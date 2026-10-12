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

# The filename match below is a short suffix test.  Requiring "/x" would add
# noise, and "/m"/"/s" would change what "$" and "." match.
## no critic (RegularExpressions::RequireExtendedFormatting, RegularExpressions::RequireLineBoundaryMatching, RegularExpressions::RequireDotMatchAnything)

use strict;
use warnings;
use bytes;
use Carp ();
use FindBin qw($Bin);
# $Bin is only known at run time, so the include path cannot be a bareword.
require "$Bin/semanticoracle.pl"; ## no critic (Modules::RequireBarewordIncludes)

my $HARNESS = defined $ARGV[0] ? $ARGV[0] : './fuzz-parse';
my $ITERS   = defined $ARGV[1] ? $ARGV[1] : 20_000;
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
			next unless $name =~ /\.[ch]$/;
			my $p = "$d/$name";
			next unless -f $p;
			open(my $fh, '<:raw', $p) or next;
			local $/ = undef;
			push @seeds, scalar <$fh>;
			close $fh or Carp::croak("$p: $!");
		}
		closedir $dh;
	}
	return \@seeds;
}

# Mutation operators.  Each takes the current data (and the seed corpus for
# the splice operator) and returns the mutated data.
my @MUTATORS = (
	# 0: flip one bit of a random byte
	sub {
		my ($data) = @_;
		my $i = int(rand(length $data));
		my $b = ord(substr($data, $i, 1)) ^ (1 << int(rand(8)));
		substr($data, $i, 1, chr($b));
		return $data;
	},
	# 1: replace a byte with a random one
	sub {
		my ($data) = @_;
		my $i = int(rand(length $data));
		substr($data, $i, 1, chr(int(rand(256))));
		return $data;
	},
	# 2: insert a random byte
	sub {
		my ($data) = @_;
		my $i = int(rand(length($data) + 1));
		substr($data, $i, 0, chr(int(rand(256))));
		return $data;
	},
	# 3: delete a byte
	sub {
		my ($data) = @_;
		my $i = int(rand(length $data));
		substr($data, $i, 1, '');
		return $data;
	},
	# 4: insert a dictionary token
	sub {
		my ($data) = @_;
		my $tok = $DICT[int(rand(@DICT))];
		my $i = int(rand(length($data) + 1));
		substr($data, $i, 0, $tok);
		return $data;
	},
	# 5: duplicate a random chunk
	sub {
		my ($data) = @_;
		if (length($data) > 1) {
			my $beg = int(rand(length $data));
			my $end = $beg + 1 + int(rand(32));
			$end = length $data if $end > length $data;
			my $chunk = substr($data, $beg, $end - $beg);
			my $i = int(rand(length($data) + 1));
			substr($data, $i, 0, $chunk);
		}
		return $data;
	},
	# 6: splice another seed at a random cut point
	sub {
		my ($data, $seeds) = @_;
		my $other = $seeds->[int(rand(@$seeds))];
		my $cut = int(rand(length($data) + 1));
		return substr($data, 0, $cut) . $other;
	},
);

sub mutate {
	my ($data, $seeds) = @_;
	my $ops = 1 + int(rand(8));
	for (1 .. $ops) {
		$data = "int x;\n" unless length $data;
		$data = $MUTATORS[int(rand(@MUTATORS))]->($data, $seeds);
	}
	return $data;
}

# Run $cmd with $data on stdin. Returns (exitcode, signal).
sub run_stdin {
	my ($cmd, $data, $wrk) = @_;
	open(my $fh, '>:raw', "$wrk/in") or Carp::croak("$wrk/in: $!");
	print {$fh} $data or Carp::croak("$wrk/in: $!");
	close $fh or Carp::croak("$wrk/in: $!");
	my $st = system("timeout $TIMEOUT $cmd <$wrk/in >$wrk/out 2>/dev/null");
	return (($st >> 8), ($st & 127));
}

sub save {
	my ($path, $data) = @_;
	open(my $fh, '>:raw', $path) or Carp::croak("$path: $!");
	print {$fh} $data or Carp::croak("$path: $!");
	close $fh or Carp::croak("$path: $!");
	return;
}

# Run one mutated input through the harness and knfmt.  Returns a finding as
# (kind, file_suffix, message), or the empty list when the input is clean.
sub check {
	my ($data, $wrk) = @_;
	my ($code, $sig) = run_stdin($HARNESS, $data, $wrk);
	return ('CRASH', "sig$sig", "sig=$sig") if $sig != 0;
	return ('TIMEOUT', '', '') if $code == 124;
	return () unless $code == 0;

	my ($kcode, $ksig) = run_stdin($KNFMT, $data, $wrk);
	return () unless $ksig == 0 && $kcode == 0;
	open(my $of, '<:raw', "$wrk/out") or return ();
	local $/ = undef;
	my $out = <$of>;
	close $of or Carp::croak("$wrk/out: $!");
	my $r = compare($data, $out);
	return () unless defined $r;
	# The source could not be interpreted reliably (e.g. an unterminated
	# literal); not a defect.
	return ('UNSUPPORTED', '', '') if $r->[0] eq 'ORACLE_UNSUPPORTED';
	return ('SEMANTIC', $r->[0], $r->[0]);
}

my $wrk = "/tmp/opencode/fuzz/work.$$";
mkdir "/tmp/opencode/fuzz" unless -d "/tmp/opencode/fuzz";
mkdir $wrk or Carp::croak("$wrk: $!");
mkdir $OUT unless -d $OUT;
END { system("rm -rf $wrk"); }

my $seeds = load_seeds();
die "no seeds\n" unless @$seeds;
srand(20261010);

my %count = (CRASH => 0, TIMEOUT => 0, SEMANTIC => 0, UNSUPPORTED => 0);
my $execs = 0;
my $started = time();

for my $n (0 .. $ITERS - 1) {
	my $data = mutate($seeds->[int(rand(@$seeds))], $seeds);
	$execs++;

	my ($kind, $suffix, $msg) = check($data, $wrk);
	next unless defined $kind;
	$count{$kind}++;

	my $name = lc($kind) . "-$n" . ($suffix eq '' ? '' : "-$suffix") . ".bin";
	save("$OUT/$name", $data);
	next if $kind eq 'UNSUPPORTED';
	print "$kind" . ($msg eq '' ? '' : " $msg") . " $OUT/$name\n";
}

my $dur = time() - $started;
printf "target=%s seeds=%d execs=%d crashes=%d timeouts=%d semantic=%d unsupported=%d duration=%.1fs\n",
    $HARNESS, scalar(@$seeds), $execs, $count{CRASH}, $count{TIMEOUT},
    $count{SEMANTIC}, $count{UNSUPPORTED}, $dur;
exit(($count{CRASH} || $count{TIMEOUT} || $count{SEMANTIC}) ? 1 : 0);
