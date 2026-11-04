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

use Archive::Tar ();
use Cwd qw(abs_path);
use Digest::SHA qw(sha256_hex);
use Errno qw(EINTR);
use File::Basename qw(dirname);
use File::Copy qw(copy);
use File::Path qw(make_path);
use File::Spec;
use File::Temp ();
use POSIX qw(_exit);
use Test::More;

my $root = abs_path(File::Spec->catdir(dirname(__FILE__), '..'));
my $work = File::Temp->newdir('knfmt-helper.XXXXXX', DIR => $ENV{TMPDIR} || '/tmp', CLEANUP => 1);
my $serial = 0;

sub read_bytes {
    my ($path) = @_;
    open my $in, '<:raw', $path or die "$path: $!\n";
    local $/;
    my $text = <$in>;
    close $in or die "$path: $!\n";
    return $text // '';
}

sub write_bytes {
    my ($path, $text) = @_;
    open my $out, '>:raw', $path or die "$path: $!\n";
    print {$out} $text or die "$path: $!\n";
    close $out or die "$path: $!\n";
}

sub run {
    my ($cwd, @command) = @_;
    my $out = File::Spec->catfile($work->dirname, 'out' . ++$serial);
    my $err = File::Spec->catfile($work->dirname, 'err' . $serial);
    my $pid = fork();
    die "fork: $!\n" unless defined $pid;
    if ($pid == 0) {
        chdir $cwd or _exit(126);
        open STDOUT, '>:raw', $out or _exit(126);
        open STDERR, '>:raw', $err or _exit(126);
        # This test invokes make independently of an outer bmake jobserver.
        delete @ENV{qw(MAKEFLAGS MFLAGS MAKELEVEL MAKEOVERRIDES)};
        exec { $command[0] } @command or _exit(127);
    }
    while (waitpid($pid, 0) == -1) {
        next if $! == EINTR;
        die "waitpid: $!\n";
    }
    my $status = $?;
    return {code => $status & 127 ? 128 + ($status & 127) : $status >> 8,
        out => read_bytes($out), err => read_bytes($err)};
}

my $bin = File::Spec->catdir($work->dirname, 'mock bin');
my $scratch = File::Spec->catdir($work->dirname, 'scratch space');
make_path($bin, $scratch);
my $mock = File::Spec->catfile($bin, 'knfmt');
write_bytes($mock, "#!/bin/sh\ncat \"\$1\"\n");
chmod 0700, $mock or die "chmod: $!\n";
local $ENV{KNFMT} = $mock;
local $ENV{TMPDIR} = $scratch;

{
    local $ENV{TMPDIR} = File::Spec->catdir($scratch, 'does-not-exist');
    my $result = run($root, 'sh', File::Spec->catfile($root, 'tests', 'bench.sh'), '2');
    is($result->{code}, 1, 'temporary directory failure is a controlled error');
    is($result->{out}, '', 'temporary directory failure produces no benchmark data');
    like($result->{err}, qr/cannot create temporary directory/,
        'temporary directory failure explains the cause');
}

for my $spec (['bench.sh', 15, 'out'], ['bench-decl.sh', 30, 'ratio']) {
    my ($script, $size, $field) = @$spec;
    my $result = run($root, 'sh', File::Spec->catfile($root, 'tests', $script), '2');
    is($result->{code}, 0, "$script succeeds with a formatter path containing spaces");
    like($result->{out}, qr/\AN=2\s+in=$size\s+.*time=[0-9]+\.[0-9]{3}\s+rssKB=(?:-|[0-9]+)\s+.*idem=ok\n\z/,
        "$script retains its output fields, input size and idempotence result");
    like($result->{out}, qr/\b$field=/, "$script retains its mode-specific field");
    is($result->{err}, '', "$script keeps stderr empty on success");
}
my $split = run($root, 'sh', File::Spec->catfile($root, 'tests', 'bench.sh'), '1 2');
is(scalar(split /\n/, $split->{out}), 2, 'expression wrapper retains a quoted list of counts');
for my $spec (['bench.sh', [1000, 2000, 4000, 8000, 16000, 32000, 64000, 100000, 200000]],
    ['bench-decl.sh', [320, 640, 1280, 2560, 5120, 10240]]) {
    my ($script, $expected) = @$spec;
    my $defaults = run($root, 'sh', File::Spec->catfile($root, 'tests', $script));
    my @counts = $defaults->{out} =~ /^N=([0-9]+)/mg;
    is_deeply(\@counts, $expected, "$script preserves its default counts");
}
my $chunk = run($root, 'sh', File::Spec->catfile($root, 'tests', 'bench.sh'), '8193');
like($chunk->{out}, qr/\AN=8193\s+in=16397\s+out=16397\s+/, 'streaming expression generation preserves chunk-boundary bytes');
write_bytes(File::Spec->catfile($work->dirname, 'knfmt'), "#!/bin/sh\ncat \"\$1\"\n");
chmod 0700, File::Spec->catfile($work->dirname, 'knfmt') or die "chmod fallback: $!\n";
{
    local $ENV{KNFMT} = File::Spec->catfile($work->dirname, 'missing');
    my $fallback = run($work->dirname, 'sh', File::Spec->catfile($root, 'tests', 'bench.sh'), '2');
    like($fallback->{out}, qr/idem=ok\n\z/, 'an unavailable KNFMT falls back to the caller-relative ./knfmt');
}
my $bad = run($root, 'sh', File::Spec->catfile($root, 'tests', 'bench.sh'), 'bad');
is($bad->{code}, 2, 'invalid count is rejected before generation');
like($bad->{err}, qr/invalid nonnegative count/, 'invalid count has a diagnostic');
write_bytes($mock, "#!/bin/sh\nexit 1\n");
my $failed = run($root, 'sh', File::Spec->catfile($root, 'tests', 'bench.sh'), '2');
is($failed->{code}, 0, 'a failed benchmark entry does not abort the remaining campaign');
is($failed->{out}, "N=2 FAILED\n", 'failed benchmark entry retains its marker');
write_bytes($mock, "#!/bin/sh\ncat \"\$1\"\nprintf ' '\n");
my $drift = run($root, 'sh', File::Spec->catfile($root, 'tests', 'bench.sh'), '2');
like($drift->{out}, qr/idem=DIFF\n\z/, 'a changing second output is reported');
opendir my $temporary, $scratch or die "opendir: $!\n";
my @left = grep { $_ ne '.' && $_ ne '..' } readdir $temporary;
closedir $temporary or die "closedir: $!\n";
is_deeply(\@left, [], 'benchmark temporary directories are removed on success and failure');

my $repo = File::Spec->catdir($work->dirname, 'release');
make_path(File::Spec->catdir($repo, 'scripts'));
copy(File::Spec->catfile($root, 'scripts', 'dist.sh'), File::Spec->catfile($repo, 'scripts', 'dist.sh'))
    or die "copy dist.sh: $!\n";
write_bytes(File::Spec->catfile($repo, 'README'), "release payload\n");
write_bytes(File::Spec->catfile($repo, 'config.mk'), '');
my @git = ('git', '-c', 'core.hooksPath=' . File::Spec->devnull,
    '-c', 'commit.gpgSign=false', '-c', 'tag.gpgSign=false',
    '-c', 'user.name=knfmt test', '-c', 'user.email=knfmt-test@example.invalid');
for my $args (['init', '-q'], ['add', 'README'], ['commit', '-qm', 'release fixture']) {
    my $result = run($repo, @git, @$args);
    is($result->{code}, 0, "release fixture git @$args") or diag($result->{err});
}
my ($version) = read_bytes(File::Spec->catfile($root, 'Makefile')) =~ /^VERSION\s*=\s*(\S+)/m;
die "Makefile version not found\n" unless defined $version;
is(run($repo, @git, 'tag', "v$version")->{code}, 0, 'release fixture has the required tag');
my @make = ('make', '-f', File::Spec->catfile($root, 'Makefile'),
    ".CURDIR=$repo", ".OBJDIR=$repo", 'dist');
my $dist = run($repo, 'sh', File::Spec->catfile($root, 'scripts', 'dist.sh'), $version);
is($dist->{code}, 0, 'standalone dist helper succeeds') or diag($dist->{err});
SKIP: {
    skip 'the existing Makefile include syntax does not support whitespace paths', 1
      if $repo =~ /\s/;
    my $made = run($repo, @make);
    is($made->{code}, 0, 'make dist delegates to the standalone helper') or diag($made->{err});
}
my $archive = File::Spec->catfile($repo, "knfmt-$version.tar.gz");
my $digest = sha256_hex(read_bytes($archive));
like(read_bytes(File::Spec->catfile($repo, "knfmt-$version.sha256")), qr/\Q$digest\E/,
    'release checksum covers the produced archive');
my $tar = Archive::Tar->new($archive, 1);
is($tar->get_content("knfmt-$version/README"), "release payload\n", 'release prefix and tagged contents are preserved');
is(run($repo, 'sh', File::Spec->catfile($root, 'scripts', 'dist.sh'), $version)->{code},
    0, 'repeating dist succeeds');
is(sha256_hex(read_bytes($archive)), $digest, 'repeated archive bytes are reproducible');
my $invalid = run($repo, 'sh', File::Spec->catfile($root, 'scripts', 'dist.sh'), '../bad');
is($invalid->{code}, 2, 'unsafe release version is rejected before invoking Git');

my $spaced = File::Spec->catdir($work->dirname, 'release with spaces');
rename $repo, $spaced or die "rename release fixture: $!\n";
is(run($spaced, 'sh', File::Spec->catfile($root, 'scripts', 'dist.sh'), $version)->{code},
    0, 'the helper supports a working directory containing spaces');

done_testing();
