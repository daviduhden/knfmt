#!/usr/bin/env perl
# Copyright (c) 2026 David Uhden Collado <daviduhden@gmail.com>
#
# Permission to use, copy, modify, and distribute this software for any
# purpose with or without fee is hereby granted, provided that the above
# copyright notice and this permission notice appear in all copies.
#
# THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
# WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
# MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
# ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
# WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
# ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
# OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.

use 5.020;
use strict;
use warnings;

use Errno qw(EINTR);
use File::Compare qw(compare);
use File::Spec;
use File::Temp ();
use POSIX qw(_exit);
use Time::HiRes qw(time);

my $mode = shift @ARGV // '';
if ($mode ne 'expression' && $mode ne 'declaration') {
    print STDERR "usage: benchmark.pl expression|declaration [counts ...]\n";
    exit 2;
}
my @counts = $mode eq 'expression' ? split(/\s+/, join(' ', @ARGV)) : @ARGV;
@counts = $mode eq 'expression'
  ? (1000, 2000, 4000, 8000, 16000, 32000, 64000, 100000, 200000)
  : (320, 640, 1280, 2560, 5120, 10240) unless @counts;
for my $count (@counts) {
    if ($count !~ /\A[0-9]+\z/) {
        print STDERR "benchmark: invalid nonnegative count: $count\n";
        exit 2;
    }
}
my $formatter = $ENV{KNFMT} // './knfmt';
$formatter = './knfmt' if $formatter eq '' || !-x $formatter;
my $tmpdir = $ENV{TMPDIR} // '/tmp';
$tmpdir = '/tmp' if $tmpdir eq '';
my $work = eval { File::Temp->newdir('knfmt.XXXXXX', DIR => $tmpdir, CLEANUP => 1) };
if (!defined $work) {
    print STDERR "benchmark: cannot create temporary directory: $@";
    exit 1;
}
my $input = File::Spec->catfile($work->dirname, $mode eq 'expression' ? 'in.c' : 'd.c');
my $first = File::Spec->catfile($work->dirname, 'o1');
my $second = File::Spec->catfile($work->dirname, 'o2');
my $metrics = File::Spec->catfile($work->dirname, 'rss');
my $null = File::Spec->devnull;
$SIG{HUP} = sub { exit 129 };
$SIG{INT} = sub { exit 130 };
$SIG{TERM} = sub { exit 143 };
$SIG{PIPE} = sub { exit 141 };

sub run_to_file {
    my ($output, $errors, @command) = @_;
    my $pid = fork();
    die "benchmark: fork: $!\n" unless defined $pid;
    if ($pid == 0) {
        open STDOUT, '>:raw', $output or _exit(127);
        open STDERR, '>:raw', $errors or _exit(127);
        exec { $command[0] } @command or _exit(127);
    }
    while (waitpid($pid, 0) == -1) {
        next if $! == EINTR;
        die "benchmark: waitpid: $!\n";
    }
    return $? == 0;
}

sub measure_rss {
    # Keep the existing BSD -l conversion. Without -l, retry and report '-'.
    if (-x '/usr/bin/time' && run_to_file($first, $metrics, '/usr/bin/time', '-l', $formatter, $input)) {
        open my $report, '<:raw', $metrics or die "benchmark: $metrics: $!\n";
        my $rss = '-';
        while (my $line = <$report>) {
            if ($line =~ /\A\s*([0-9]+)\s+maximum resident set size\b/) {
                $rss = int($1 / 1024);
                last;
            }
        }
        close $report or die "benchmark: $metrics: $!\n";
        return $rss;
    }
    return run_to_file($first, $null, $formatter, $input) ? '-' : undef;
}

sub generate_input {
    my ($count) = @_;
    open my $out, '>:raw', $input or die "benchmark: $input: $!\n";
    if ($mode eq 'expression') {
        print {$out} 'int x = ' or die "benchmark: $input: $!\n";
        # Bound temporary memory independently of the requested expression size.
        while ($count >= 8192) {
            print {$out} 'a+' x 8192 or die "benchmark: $input: $!\n";
            $count -= 8192;
        }
        print {$out} ('a+' x $count), "a;\n" or die "benchmark: $input: $!\n";
    } else {
        for (my $i = 0; $i < $count; $i++) {
            print {$out} "int f$i () A B;\n" or die "benchmark: $input: $!\n";
        }
    }
    close $out or die "benchmark: $input: $!\n";
}

my $previous;
for my $count (@counts) {
    generate_input($count);
    my $bytes = -s $input;
    my $start = time;
    my $rss = measure_rss();
    if (!defined $rss) {
        print "N=$count FAILED\n";
        next;
    }
    my $elapsed = sprintf '%.3f', time - $start;
    run_to_file($second, $null, $formatter, $first);
    my $idempotent = compare($first, $second) == 0 ? 'ok' : 'DIFF';
    if ($mode eq 'expression') {
        printf "N=%-7s in=%-9s out=%-9s time=%-8s rssKB=%-8s idem=%s\n",
          $count, $bytes, -s $first, $elapsed, $rss, $idempotent;
    } else {
        my $ratio = defined($previous) && $previous != 0
          ? sprintf('%.2f', $elapsed / $previous) : '-';
        printf "N=%-6s in=%-8s time=%-8s rssKB=%-7s ratio=%-5s idem=%s\n",
          $count, $bytes, $elapsed, $rss, $ratio, $idempotent;
        $previous = $elapsed;
    }
}
