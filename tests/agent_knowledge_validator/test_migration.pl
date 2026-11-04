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
# WHATSOEVER RESULTING FROM LOSS OF USE, DATA, OR PROFITS, WHETHER IN AN
# ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
# OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.

use strict;
use warnings;
use utf8;

use Encode qw(encode);
use File::Spec;
use FindBin          qw($Bin);
use JSON::PP         ();
use Text::ParseWords qw(parse_line);
use Test::More;

require "$Bin/test_validator.pl";

binmode STDOUT, ':encoding(UTF-8)' or die "cannot set TAP output mode: $!";
binmode STDERR, ':encoding(UTF-8)' or die "cannot set TAP error mode: $!";

subtest 'historical infrastructure filenames retain their exemption' => sub {
    my $fixture = new_repository();
    # A historical pathname is fixture data, not an executable dependency.
    write_file( $fixture->{repo}, 'scripts/validate-agent-knowledge.py',
        "retired infrastructure fixture\n" );
    my $result = run_validator( $fixture->{repo}, '--scope', 'worktree' );
    is( $result->{status}, 0, $result->{stdout} . $result->{stderr} );
};

sub json_report {
    my ($result) = @_;
    my $report = eval { JSON::PP->new->decode( $result->{stdout} ) };
    ok( defined $report, 'stdout contains one JSON document' )
      or diag $result->{stdout} . $result->{stderr};
    return $report;
}

subtest 'Unicode paths and shell metacharacters remain literal' => sub {
    my $fixture    = new_repository();
    my $previous   = $fixture->{repo};
    my $repository = File::Spec->catdir(
        $fixture->{temporary}->dirname,
        'répo con espacios; $(touch injected)'
    );
    rename $previous, encode( 'UTF-8', $repository )
      or die "cannot rename fixture: $!";
    $fixture->{repo} = $repository;
    my $relative = 'src/espacio ñ 日本語; $(touch injected).txt';
    write_file( $repository, $relative, "opaque data\n" );
    my $result =
      run_validator( $repository, '--scope', 'worktree', '--format', 'json' );
    is( $result->{status}, 1,
        'untracked Unicode path needs a knowledge update' );
    is( $result->{stderr}, '', 'valid Unicode causes no interpreter warning' );
    my $report = json_report($result);
    return unless defined $report;
    my ($check) = grep { $_->{rule_id} eq 'AK001' } @{ $report->{checks} };
    ok(
        ( grep { $_ eq $relative } @{ $check->{files} } ),
        'JSON contains the complete Unicode path'
    );
    ok( !-e File::Spec->catfile( encode( 'UTF-8', $repository ), 'injected' ),
        'path text was never executed by a shell' );
};

subtest
  'Unicode repository roots and instruction directories resolve literally' =>
  sub {
    my $fixture    = new_repository();
    my $repository = File::Spec->catdir( $fixture->{temporary}->dirname,
        'repositorio ñ con espacios [x]; literal' );
    rename $fixture->{repo}, encode( 'UTF-8', $repository )
      or die "cannot rename fixture: $!";
    $fixture->{repo} = $repository;
    update_agent($repository);
    my $root_agent =
      read_file( File::Spec->catfile( $repository, 'AGENTS.md' ) );
    write_file( $repository, 'guías/README.md', "Documentation fixture.\n" );
    write_file( $repository, 'guías/AGENTS.md',
        $root_agent
          . "\nEstas instrucciones cubren las guías y sus ejemplos de compatibilidad.\n"
    );
    my $result =
      run_validator( $repository, '--scope', 'worktree', '--format', 'json' );
    is( $result->{status}, 0,  $result->{stdout} . $result->{stderr} );
    is( $result->{stderr}, '', 'Unicode roots do not generate warnings' );
    my $report = json_report($result);
    return unless defined $report;
    my ($check) = grep { $_->{rule_id} eq 'AK004' } @{ $report->{checks} };
    ok(
        ( grep { $_ eq 'guías/AGENTS.md' } @{ $check->{files} } ),
        'the nested Unicode instruction is discovered'
    );
  };

subtest 'percent-encoded UTF-8 links decode complete byte sequences' => sub {
    my $fixture = new_repository();
    write_file( $fixture->{repo}, 'docs/niño.txt',
        "Referenced Unicode document.\n" );
    my $path = File::Spec->catfile( $fixture->{repo}, 'AGENTS.md' );
    write_file( $fixture->{repo}, 'AGENTS.md',
        read_file($path)
          . "\nReview [documentación](docs/ni%C3%B1o.txt) when updating the parser.\n"
    );
    my $result =
      run_validator( $fixture->{repo}, '--scope', 'worktree', '--format',
        'json' );
    is( $result->{status}, 0, $result->{stdout} . $result->{stderr} );
    my $report = json_report($result);
    return unless defined $report;
    my ($check) = grep { $_->{rule_id} eq 'AK003' } @{ $report->{checks} };
    is( $check->{status}, 'PASS',
        'encoded link resolves to the real Unicode filename' );
};

subtest 'decoded Unicode command arguments are not decoded twice' => sub {
    my $fixture = new_repository();
    git_ok( $fixture->{repo}, 'branch', 'rama-ñ' );
    local $ENV{PERL_UNICODE} = 'A';
    my $result = run_validator(
        $fixture->{repo}, '--scope', 'range', '--base',
        'rama-ñ',         '--head',  'HEAD',  '--format',
        'json'
    );
    is( $result->{status}, 0,  $result->{stdout} . $result->{stderr} );
    is( $result->{stderr}, '', 'already-decoded arguments cause no warning' );
    json_report($result);
};

subtest 'execution failures keep stdout machine-readable' => sub {
    my $fixture = new_repository();
    my $result  = run_validator(
        $fixture->{repo},   '--scope', 'range', '--base',
        'missing-revision', '--head',  'HEAD',  '--format',
        'json'
    );
    is( $result->{status}, 2, 'invalid revision is an execution error' );
    my $report = json_report($result);
    return unless defined $report;
    is( $report->{status}, 'FAIL', 'JSON reports failure' );
    is( $report->{errors}[0]{rule_id}, 'AK005',
        'execution failure uses AK005' );
    like(
        $result->{stderr},
        qr/validator execution error:/,
        'execution detail goes to stderr'
    );
};

subtest 'CLI option forms preserve usage errors' => sub {
    my $fixture = new_repository();
    for my $case (
        [ [],                       qr/required.*--scope/ ],
        [ [ '--scope', 'unknown' ], qr/invalid choice/ ],
        [
            [ '--scope', 'worktree', '--base', 'HEAD' ],
            qr/only valid with --scope range/
        ],
        [
            [ '--scope', 'worktree', '--format', 'unknown' ],
            qr/invalid choice/
        ],
        [
            [ '--scope', 'worktree', '--unexpected' ],
            qr/unrecognized arguments/
        ],
      )
    {
        my $result = run_validator( $fixture->{repo}, @{ $case->[0] } );
        is( $result->{status}, 2,  'invalid arguments exit 2' );
        is( $result->{stdout}, '', 'usage errors leave stdout empty' );
        like( $result->{stderr}, $case->[1],
            'usage error identifies the problem' );
    }
    my $result =
      run_validator( $fixture->{repo}, '--scope=worktree', '--format=json' );
    is( $result->{status}, 0, 'equals-form long options are accepted' );
    json_report($result);
};

subtest 'typed TOML and dotted tables configure the same rules' => sub {
    my $fixture = new_repository();
    write_file( $fixture->{repo}, '.agent-knowledge-validator.toml', <<'TOML' );
[validator]
contract_version = '1.0.0'
minimum_changed_lines = 0x1
inline_path_checks = false
exclude_paths = [
  'generated/**', # a multiline list with comments
]
[validator.required_phrases]
'AGENTS.md' = ['Keep operational guidance accurate and reusable.']
TOML
    commit_all( $fixture->{repo}, 'configure portable validator options' );
    write_file( $fixture->{repo}, 'generated/ignored.txt', "generated\n" );
    my $result =
      run_validator( $fixture->{repo}, '--scope', 'worktree', '--format',
        'json' );
    is( $result->{status}, 0, $result->{stdout} . $result->{stderr} );
    json_report($result);
};

subtest 'multiline TOML strings preserve their decoded values' => sub {
    my $fixture = new_repository();
    write_file( $fixture->{repo}, '.agent-knowledge-validator.toml', <<'TOML' );
[validator]
contract_version = """
1.0.0"""
minimum_changed_lines = 0b1
[validator.required_phrases]
'AGENTS.md' = [
  """Keep operational \
guidance accurate and reusable.""",
  '''Keep operational guidance accurate and reusable.''',
]
TOML
    commit_all( $fixture->{repo}, 'configure multiline values' );
    my $result =
      run_validator( $fixture->{repo}, '--scope', 'worktree', '--format',
        'json' );
    is( $result->{status}, 0, $result->{stdout} . $result->{stderr} );
    json_report($result);
};

subtest 'invalid configuration types and unsafe paths fail closed' => sub {
    my $fixture = new_repository();
    for my $setting (
        'exclude_paths = ["../outside/**"]',
        'exclude_paths = ["/absolute/**"]',
        'inline_path_checks = "true"',
        'minimum_changed_lines = 0',
        'minimum_changed_lines = 1001',
        'unknown_setting = true',
        'exclude_paths = [1]',
        'exclude_paths = ["unterminated]',
        "contract_version = '1.0.0'\ncontract_version = '1.0.0'",
      )
    {
        write_file( $fixture->{repo}, '.agent-knowledge-validator.toml',
            "[validator]\n$setting\n" );
        my $result =
          run_validator( $fixture->{repo}, '--scope', 'worktree', '--format',
            'json' );
        is( $result->{status}, 2, 'invalid configuration exits 2' );
        my $report = json_report($result);
        next unless defined $report;
        is( $report->{errors}[0]{rule_id},
            'AK005', 'invalid setting uses AK005' );
    }
};

subtest 'configuration globs retain character-class and Unicode behavior' =>
  sub {
    my $fixture  = new_repository();
    my $scenario = 0;
    for my $case (
        [ '[^a].txt',      '^.txt',       0 ],
        [ '[^a].txt',      'b.txt',       1 ],
        [ '[z-a].txt',     'z.txt',       1 ],
        [ '[!z-a].txt',    'z.txt',       0 ],
        [ 'file[[]x].txt', 'file[x].txt', 0 ],
        [ 'file?*.txt',    'fileé.txt',   0 ],
        [ '*.txt',         "line\n.txt",  0 ],
      )
    {
        my ( $pattern, $name, $status ) = @{$case};
        $scenario++;
        write_file( $fixture->{repo}, '.agent-knowledge-validator.toml',
            "# Scenario $scenario\n[validator]\nexclude_paths = ['$pattern']\n"
        );
        commit_all( $fixture->{repo}, 'configure character-class exclusion' );
        write_file( $fixture->{repo}, $name, "Untracked project file.\n" );
        my $result =
          run_validator( $fixture->{repo}, '--scope', 'worktree', '--format',
            'json' );
        is( $result->{status}, $status,
            "glob $pattern classifies the literal filename" );
        json_report($result);
        unlink encode( 'UTF-8', File::Spec->catfile( $fixture->{repo}, $name ) )
          or die "cannot remove glob fixture: $!";
    }
  };

subtest 'path traversal links cannot escape the repository' => sub {
    my $fixture = new_repository();
    write_file( $fixture->{temporary}->dirname, 'outside.md', "outside\n" );
    my $path = File::Spec->catfile( $fixture->{repo}, 'AGENTS.md' );
    write_file( $fixture->{repo}, 'AGENTS.md',
        read_file($path)
          . "\nSee [outside](../outside.md) for operational guidance.\n" );
    my $result =
      run_validator( $fixture->{repo}, '--scope', 'worktree', '--format',
        'json' );
    is( $result->{status}, 1, 'an existing outside target is rejected' );
    my $report = json_report($result);
    return unless defined $report;
    ok( ( grep { $_->{rule_id} eq 'AK003' } @{ $report->{errors} } ),
        'reference escape fails AK003' );
};

subtest 'nonexistent path components preserve parent traversal semantics' =>
  sub {
    for my $case (
        [ 'missing/../README.md',     0, 'PASS' ],
        [ 'missing/../../outside.md', 1, 'FAIL' ],
      )
    {
        my $fixture = new_repository();
        write_file( $fixture->{temporary}->dirname,
            'outside.md', "Outside repository fixture.\n" );
        my $path = File::Spec->catfile( $fixture->{repo}, 'AGENTS.md' );
        write_file( $fixture->{repo}, 'AGENTS.md',
            read_file($path)
              . "\nReview [the reference]($case->[0]) when updating parser boundaries.\n"
        );
        my $result = run_validator( $fixture->{repo}, '--scope', 'worktree',
            '--format', 'json' );
        is( $result->{status}, $case->[1],
            'parent traversal has the expected exit code' );
        my $report = json_report($result);
        next unless defined $report;
        my ($check) = grep { $_->{rule_id} eq 'AK003' } @{ $report->{checks} };
        is( $check->{status}, $case->[2],
            'AK003 validates the resolved destination' );
    }
  };

subtest 'range revisions are literal arguments' => sub {
    my $fixture  = new_repository();
    my $revision = 'HEAD;touch injected';
    my $result = run_validator( $fixture->{repo}, '--scope', 'range', '--base',
        $revision, '--head', 'HEAD' );
    is( $result->{status}, 2,
        'metacharacters do not turn a bad revision into a command' );
    ok( !-e File::Spec->catfile( $fixture->{repo}, 'injected' ),
        'no shell side effect' );
};

subtest 'subdirectory invocation resolves the repository root' => sub {
    my $fixture   = new_repository();
    my $directory = File::Spec->catdir( $fixture->{repo}, 'src' );
    my $result    = run_validator_in( $fixture->{repo}, $directory,
        '--scope', 'worktree', '--format', 'json' );
    is( $result->{status}, 0,
        'clean invocation from a child directory passes' );
    json_report($result);
};

subtest 'child capture drains both pipes and preserves signals' => sub {
    my $child = File::Spec->catfile( $Bin, 'fixtures', 'child-process.pl' );
    my ( $out, $err, $status ) = capture_command( $^X, $child, 'output' );
    is( length($out), 100000, 'large stdout drained' );
    is( length($err), 100000, 'large stderr drained' );
    is( $status,      7,      'nonzero child status retained' );
    ( $out, $err, $status ) = capture_command( $^X, $child, 'signal' );
    is( $status, 143, 'signal cannot be reported as successful exit' );
};

# This is a bounded policy lint, not a shell interpreter or a security proof.
# ParseWords distinguishes quoted data from command words; heredoc bodies are
# ignored as data. Computed executable names and nested shell interpreters still
# require the repository audit. Recognized forms are regression-tested below.
sub unquote_word {
    my ($raw) = @_;
    my @words = parse_line( '\s+', 0, $raw );
    return @words == 1 ? $words[0] : $raw;
}

sub inline_programs {
    my ($source) = @_;
    my ( $prepared, @heredocs ) = ('');
    for my $line ( split /\n/, $source, -1 ) {
        if (@heredocs) {
            my $candidate = $line;
            $candidate =~ s/^\t+// if $heredocs[0]{tabs};
            shift @heredocs        if $candidate eq $heredocs[0]{name};
            $prepared .= "\n";
            next;
        }
        $line = '' if $line =~ /^\s*#/;
        my @parts = parse_line( '(\s+|<<-?|[;&|()<>]+)', 'delimiters', $line );
        @parts = grep { defined $_ && $_ !~ /^\s*$/ } @parts;
        for my $index ( 0 .. $#parts - 1 ) {
            next unless $parts[$index] =~ /^<<(-?)$/;
            push @heredocs,
              {
                name => unquote_word( $parts[ $index + 1 ] ),
                tabs => $1 ne ''
              };
        }
        $prepared .= "$line\n";
    }

    # Retain quoted text while dropping trailing shell comments, even when a
    # comment itself contains unmatched quote marks.
    $prepared =~ s{("(?:\\.|[^"\\])*"|'[^']*'|\\.)|(?:^|(?<=[\s;|&()]))\#[^\n]*}
                  {defined $1 ? $1 : ''}gme;
    $prepared =~ s/\$\((PERL|PYTHON)\)/\$\{$1\}/g;
    my @parts = parse_line( '(\s+|[;&|()<>]+)', 'delimiters', $prepared );
    return ['unbalanced command quoting; inspect manually']
      if !@parts && $prepared =~ /\S/;
    my ( $start, $line ) = ( 1, 1 );
    my @found;

    for my $index ( 0 .. $#parts ) {
        my $raw = $parts[$index];
        next unless defined $raw;
        my $at_line = $line;
        $line += $raw =~ tr/\n/\n/;
        if ( $raw =~ /\n/ && $raw =~ /^\s*$/ || $raw =~ /^[;&|()]+$/ ) {
            $start = 1;
            next;
        }
        next if $raw =~ /^\s*$/;
        next unless $start;
        my $word = unquote_word($raw);
        next
          if $word =~
          /^(?:if|elif|else|then|do|while|until|!|env|command|exec|run:)$/
          || $word =~ /^\w+=/
          || $word =~ /^\$\{EXEC/
          || $word =~ /^-/;
        $start = 0;
        next
          unless $word =~ m{(?:^|/)(perl(?:[0-9.]+)?|python(?:[0-9.]+)?)$}
          || $word =~ /^\$\{?(?:PERL|PYTHON)\}?$/;
        my $family = $word =~ /(?:python|PYTHON)/ ? 'python' : 'perl';
        my ( $program, $redirection, $help, $literal ) = ( 0, 0, 0, 0 );

        for my $next ( $index + 1 .. $#parts ) {
            my $argument = $parts[$next];
            next unless defined $argument;
            last
              if $argument =~ /\n/ && $argument =~ /^\s*$/
              || $argument =~ /^[;&|()]+$/;
            next if $argument =~ /^\s*$/;
            $argument = unquote_word($argument);
            if ($redirection)                     { $redirection = 0; next; }
            if ( $argument =~ /^[<>]+-?$/ )       { $redirection = 1; next; }
            if ( !$literal && $argument eq '--' ) { $literal     = 1; next; }

            if (
                !$literal
                && (
                      $family eq 'perl'
                    ? $argument =~
                      /^-(?:[acdfhlnpsStTuUvVwWX0-9]*[eE]|-eval(?:=|$))/
                    : $argument =~
                    /^-(?:[bBdEiIOqRsSuUvVx0-9]*c|-command(?:=|$))/
                )
              )
            {
                push @found, "line $at_line: inline $family program";
                $program = 1;
                last;
            }
            $help = 1
              if !$literal && $argument =~ /^(?:--help|--version|-[hvV])$/;
            next         if !$literal && $argument =~ /^-/ && $argument ne '-';
            $program = 1 if $argument ne '-';
        }
        push @found, "line $at_line: $family program read from stdin"
          if !$program && !$help;
    }
    return \@found;
}

subtest 'inline-command policy understands executable words and quoted data' =>
  sub {
    for my $source (
        q{perl -e 'print 1'},
        q{perl -we 'print 1'},
        q{perl -E 'say 1'},
        q{env X=y /usr/bin/perl -MTime::HiRes=time -e 'print time'},
        q{time=$(perl -e 'print 1')},
        q{${PERL} -e 'print 1'},
        q{$(PERL) -e 'print 1'},
        q{python3 -c 'print(1)'},
        "perl <<'END'\nprint 1;\nEND\n",
        q{printf 'print 1' | perl},
      )
    {
        ok( @{ inline_programs($source) } > 0, 'inline program is detected' );
    }
    for my $source (
        q{perl scripts/tool.pl argument},
        q{perl -c scripts/tool.pl},
        q{perl -v},
        q{echo 'perl -e code'},
        q{printf '%s' 'perl -e code'},
        q{perl scripts/tool.pl # don't interpret 'perl -e' as code},
        "# perl -e 'documentation only'\nperl scripts/tool.pl\n",
        "cat <<'END'\nperl -e 'template text'\nEND\nperl scripts/tool.pl\n",
        "perl scripts/tool.pl <<'DATA'\nopaque input\nDATA\n",
      )
    {
        is_deeply( inline_programs($source),
            [], 'standalone tools and quoted data are accepted' );
    }
  };

subtest
  'maintained shell, build and CI files contain no inline interpreter programs'
  => sub {
    my $root   = File::Spec->rel2abs( File::Spec->catdir( $Bin, '..', '..' ) );
    my $listed = git_ok( $root, 'ls-files', '-z', '--cached', '--others',
        '--exclude-standard' );
    my ( @violations, %seen, $checked );
    for my $relative ( split /\0/, $listed ) {
        next if $seen{$relative}++;
        my $path = File::Spec->catfile( $root, split m{/}, $relative );
        next unless -f $path;
        open my $handle, '<:raw', $path or die "cannot read $path: $!";
        my $first = <$handle> // '';
        my $selected =
          $relative =~
/(?:^|\/)(?:GNUmakefile|Makefile|configure)$|\.(?:sh|ksh|bash|mk|ya?ml)$/
          || $first =~ /^#![^\n]*\b(?:sh|bash|ksh|dash|zsh)\b/;
        if ( !$selected ) { close $handle; next; }
        local $/;
        my $source = $first . ( <$handle> // '' );
        close $handle or die "cannot close $path: $!";
        $checked++;
        push @violations, map { "$relative: $_" } @{ inline_programs($source) };
    }
    ok( $checked > 0, 'maintained command files were inspected' );
    is_deeply( \@violations, [],
        'all executable programs reside in standalone files' );
  };

done_testing();
