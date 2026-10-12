#!/usr/bin/perl

# Mutation-based fuzzing driver for the knfmt fuzz harnesses (Perl).
#
# Feeds mutated inputs to a standalone harness (built in the default AFL mode,
# which reads one input from stdin) and classifies findings:
#
#   CRASH              killed by a signal (e.g. the harness trap)
#   TIMEOUT            exceeded the per-input limit
#   SEMANTIC_*         formatting changed the program's preprocessing tokens
#
# The harness must enforce the idempotence property itself (fuzz-parse traps
# on non-idempotent formatting).  The semantic check uses the independent
# oracle in tests/semanticoracle.pl.
#
# Usage: perl tests/mutate.pl <harness> [iterations] [outdir]

use strict;
use warnings;
use bytes;
use FindBin qw($Bin);
require "$Bin/semanticoracle.pl";

my $HARNESS = defined $ARGV[0] ? $ARGV[0] : './fuzz-parse';
my $ITERS   = defined $ARGV[1] ? $ARGV[1] : 20000;
my $OUT     = defined $ARGV[2] ? $ARGV[2] : 'crashes';
my $KNFMT   = defined $ENV{KNFMT} ? $ENV{KNFMT} : './knfmt';
my $TIMEOUT = 15;
my @DIRS    = ('tests');

my @DICT = (
	"\n", "\t", " ", "/*", "*/", "//", "//\n", "\\\n",
	"#if", "#ifdef", "#ifndef", "#else", "#elif", "#endif", "#define",
	"#include", "#undef", "#error", "(", ")", "{", "}", "[", "]",
	";", ",", "|", "<", ">", "||", "&&", "?", ":", "\"", "'",
	"0", "8", "3600", "0xffffffff", "18446744073709551615", "->",
	"...", "sizeof", "_Generic", "_Static_assert", "static_assert",
	"__attribute__", "asm", "static", "struct", "union", "enum",
	"NULL", "nullptr", "\x00", "\xff", "\r\n", "\r", "A" x 64,
	"??/\n", "??=", "??'", "??!", "in", "main", "voiint",
);

sub load_seeds {
	my @seeds;
	for my $d (@DIRS) {
		opendir(my $dh, $d) or next;
		for my $name (readdir $dh) {
			next unless $name =~ /\.(c|h)$/;
			my $p = "$d/$name";
			next unless -f $p;
			open(my $fh, '<:raw', $p) or next;
			local $/;
			push @seeds, scalar <$fh>;
			close $fh;
		}
		closedir $dh;
	}
	return \@seeds;
}

sub mutate {
	my ($data, $seeds) = @_;
	my $ops = 1 + int(rand(8));
	for (1 .. $ops) {
		my $op = int(rand(7));
		$data = "int x;\n" unless length $data;
		if ($op == 0) {
			my $i = int(rand(length $data));
			my $b = ord(substr($data, $i, 1)) ^ (1 << int(rand(8)));
			substr($data, $i, 1) = chr($b);
		} elsif ($op == 1) {
			my $i = int(rand(length $data));
			substr($data, $i, 1) = chr(int(rand(256)));
		} elsif ($op == 2) {
			my $i = int(rand(length($data) + 1));
			substr($data, $i, 0) = chr(int(rand(256)));
		} elsif ($op == 3) {
			my $i = int(rand(length $data));
			substr($data, $i, 1) = '';
		} elsif ($op == 4) {
			my $tok = $DICT[int(rand(@DICT))];
			my $i = int(rand(length($data) + 1));
			substr($data, $i, 0) = $tok;
		} elsif ($op == 5) {
			if (length($data) > 1) {
				my $a = int(rand(length $data));
				my $b = $a + 1 + int(rand(32));
				$b = length $data if $b > length $data;
				my $chunk = substr($data, $a, $b - $a);
				my $i = int(rand(length($data) + 1));
				substr($data, $i, 0) = $chunk;
			}
		} else {
			my $other = $seeds->[int(rand(@$seeds))];
			my $cut = int(rand(length($data) + 1));
			$data = substr($data, 0, $cut) . $other;
		}
	}
	return $data;
}

# Run $cmd with $data on stdin. Returns (exitcode, signal).
sub run_stdin {
	my ($cmd, $data, $wrk) = @_;
	open(my $fh, '>:raw', "$wrk/in") or die "$wrk/in: $!";
	print $fh $data;
	close $fh;
	my $st = system("timeout $TIMEOUT $cmd <$wrk/in >$wrk/out 2>/dev/null");
	return (($st >> 8), ($st & 127));
}

sub save {
	my ($path, $data) = @_;
	open(my $fh, '>:raw', $path) or die "$path: $!";
	print $fh $data;
	close $fh;
}

my $wrk = "/tmp/opencode/fuzz/work.$$";
mkdir "/tmp/opencode/fuzz" unless -d "/tmp/opencode/fuzz";
mkdir $wrk or die "$wrk: $!";
mkdir $OUT unless -d $OUT;
END { system("rm -rf $wrk"); }

my $seeds = load_seeds();
die "no seeds\n" unless @$seeds;
srand(20261010);

my ($execs, $crashes, $timeouts, $semantic, $unsupported) = (0, 0, 0, 0, 0);
my $started = time();

for my $n (0 .. $ITERS - 1) {
	my $data = mutate($seeds->[int(rand(@$seeds))], $seeds);
	$execs++;

	my ($code, $sig) = run_stdin($HARNESS, $data, $wrk);
	if ($sig != 0) {
		$crashes++;
		my $p = "$OUT/crash-$n-sig$sig.bin";
		save($p, $data);
		print "CRASH sig=$sig $p\n";
		next;
	}
	if ($code == 124) {
		$timeouts++;
		my $p = "$OUT/timeout-$n.bin";
		save($p, $data);
		print "TIMEOUT $p\n";
		next;
	}
	next unless $code == 0;

	my ($kcode, $ksig) = run_stdin($KNFMT, $data, $wrk);
	next unless $ksig == 0 && $kcode == 0;
	open(my $of, '<:raw', "$wrk/out") or next;
	local $/;
	my $out = <$of>;
	close $of;
	my $r = compare($data, $out);
	next unless defined $r;
	if ($r->[0] eq 'ORACLE_UNSUPPORTED') {
		# The source could not be interpreted reliably (e.g. an
		# unterminated literal); not a defect.
		$unsupported++;
		save("$OUT/unsupported-$n.bin", $data);
		next;
	}
	$semantic++;
	my $p = "$OUT/semantic-$n-$r->[0].bin";
	save($p, $data);
	print "SEMANTIC $r->[0] $p\n";
}

my $dur = time() - $started;
printf "target=%s seeds=%d execs=%d crashes=%d timeouts=%d semantic=%d unsupported=%d duration=%.1fs\n",
    $HARNESS, scalar(@$seeds), $execs, $crashes, $timeouts, $semantic,
    $unsupported, $dur;
exit(($crashes || $timeouts || $semantic) ? 1 : 0);
