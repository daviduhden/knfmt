#!/usr/bin/perl

# Independent semantic-preservation oracle for knfmt (Perl).
#
# Formats change layout but must not change the program's preprocessing
# tokens.  This module tokenizes two byte strings (translation phases 2 and 3)
# and compares them.  It does not use knfmt's lexer.
#
# Policy
#   * Translation phase 1 (trigraph replacement) and phase 2
#     (backslash-newline splicing) are applied first.
#   * Preprocessing tokens: identifiers, preprocessing numbers, string and
#     character literals (escape aware), punctuators (longest match), comments
#     and newlines.
#   * Every non-comment token, including stray backslashes and semicolons,
#     is preserved. Directive boundaries and function-like macro adjacency
#     are significant.
#   * The resulting non-comment token sequence must be identical, which
#     detects token loss, duplication, reordering, operator/punctuator and
#     literal changes, directive changes and branch swaps.
#   * Comments may be relocated and reindented, so only their
#     whitespace-normalized spelling multiset is compared.
#   * An unterminated literal (a raw newline inside a string or character
#     literal) cannot be interpreted as a preprocessing token; the comparison
#     reports ORACLE_UNSUPPORTED instead of comparing undefined bytes.
#
# Run with --selftest to check the oracle itself.

# The regular expressions below are token scans: they match a few characters,
# not whole lines.  Requiring "/x" would add noise for one-character classes
# and would change the meaning of "[ \t\x0b\f\r]"; requiring "/m" or "/s" would
# change what "^"/"$" and "." match.
## no critic (RegularExpressions::RequireExtendedFormatting, RegularExpressions::RequireLineBoundaryMatching, RegularExpressions::RequireDotMatchAnything)

use strict;
use warnings;
use bytes;
use Digest::SHA qw(sha256_hex);
use FindBin qw($Bin);

my @PUNCT = sort { length($b) <=> length($a) } (
    '...', '<<=', '>>=', '->', '++', '--', '<<', '>>', '<=', '>=',
    '==',  '!=',  '&&',  '||', '*=', '/=', '%=', '+=', '-=', '&=',
    '^=',  '|=',  '##',  '[',  ']',  '(',  ')',  '{',  '}',  '.',
    '&',   '*',   '+',   '-',  '~',  '!',  '/',  '%',  '<',  '>',
    '^',   '|',   '?',   ':',  ';',  '=',  ',',  '#',
);

sub tokenize {
    my ($d) = @_;

    # Translation phase 1: trigraph replacement.
    $d =~ s/\?\?=/#/g;
    $d =~ s/\?\?\//\\/g;
    $d =~ s/\?\?'/^/g;
    $d =~ s/\?\?\(/[/g;
    $d =~ s/\?\?\)/]/g;
    $d =~ s/\?\?!/|/g;
    $d =~ s/\?\?</{/g;
    $d =~ s/\?\?>/}/g;
    $d =~ s/\?\?-/~/g;

    # Translation phase 2: remove every backslash-newline.
    $d =~ s/\\\r?\n//g;

    my $n = length($d);
    my $i = 0;
    my @t;
    while ( $i < $n ) {
        my $c = substr( $d, $i, 1 );
        if ( $c =~ /[ \t\x0b\f\r]/ ) { $i++; next; }
        if ( $c eq "\n" ) { push @t, [ 'nl', "\n" ]; $i++; next; }
        if ( $c eq '/' ) {
            my ( $tk, $j ) = _scan_comment( $d, $i, $n );
            if ( defined $tk ) {
                push @t, $tk;
                $i = $j;
                next;
            }
        }
        my $prefix = '';
        if ( $c =~ /[uUL]/ && substr( $d, $i, 3 ) =~ /\A(u8|u|U|L)(?=["'])/ ) {
            $prefix = $1;
        }
        if ( $prefix ne '' || $c eq '"' || $c eq "'" ) {
            my ( $tk, $j ) = _scan_literal( $d, $i + length($prefix), $n );
            $tk->[1] = $prefix . $tk->[1];
            push @t, $tk;
            $i = $j;
            next;
        }
        if ( $c eq '\\' ) { push @t, [ 'backslash', '\\' ]; $i++; next; }
        if ( $c =~ /[A-Za-z_]/ ) {
            my $j = $i + 1;
            $j++ while $j < $n && substr( $d, $j, 1 ) =~ /[A-Za-z0-9_]/;
            push @t, [ 'ident', substr( $d, $i, $j - $i ), substr( $d, $j, 1 ) eq '(' ];
            $i = $j;
            next;
        }
        if ( $c =~ /[0-9]/
            || ( $c eq '.' && substr( $d, $i + 1, 1 ) =~ /[0-9]/ ) )
        {
            my ( $tk, $j ) = _scan_number( $d, $i, $n );
            push @t, $tk;
            $i = $j;
            next;
        }
        my ( $tk, $j ) = _scan_punct( $d, $i );
        if ( defined $tk ) {
            push @t, $tk;
            $i = $j;
            next;
        }
        push @t, [ 'other', $c ];
        $i++;
    }
    return \@t;
}

# Scan a block comment (`/*`) or line comment (`//`) at $i.  Returns a
# ('comment', text) token and the index just past it, or an empty list when
# $i does not start a comment.
sub _scan_comment {
    my ( $d, $i, $n ) = @_;
    my $next = substr( $d, $i + 1, 1 );
    my $j;
    if ( $next eq '*' ) {
        $j = index( $d, '*/', $i + 2 );
        $j = ( $j < 0 ) ? $n : $j + 2;
    }
    elsif ( $next eq '/' ) {
        $j = index( $d, "\n", $i + 2 );
        $j = ( $j < 0 ) ? $n : $j;
    }
    else {
        return;
    }
    return [ 'comment', substr( $d, $i, $j - $i ) ], $j;
}

# Scan a string or character literal at $i.  A raw newline inside it is a
# constraint violation, so such a literal is reported as 'uliteral'.  Returns
# a (type, text) token and the index just past it.
sub _scan_literal {
    my ( $d, $i, $n ) = @_;
    my $q = substr( $d, $i, 1 );
    my $j = $i + 1;
    while ( $j < $n ) {
        my $ch = substr( $d, $j, 1 );
        last if $ch eq $q;
        if ( $ch eq '\\' ) { $j += 2; next; }
        $j++;
    }
    my $closed = $j < $n && substr( $d, $j, 1 ) eq $q;
    $j = $n if $j > $n;
    $j++;
    $j = $n if $j > $n;
    my $lit = substr( $d, $i, $j - $i );
    return [ ( !$closed || $lit =~ /\n/ ? 'uliteral' : 'literal' ), $lit ], $j;
}

# Scan a preprocessing number at $i.  Returns a (type, text) token and the
# index just past it.
sub _scan_number {
    my ( $d, $i, $n ) = @_;
    my $j = $i + 1;
    while ( $j < $n ) {
        my $ch = substr( $d, $j, 1 );
        if ( $ch =~ /[eEpP]/
            && substr( $d, $j + 1, 1 ) =~ /[+\-]/ )
        {
            $j += 2;
            next;
        }
        if ( $ch =~ /[A-Za-z0-9_.]/ ) { $j++; next; }
        if ( $ch eq "'" && substr( $d, $j + 1, 1 ) =~ /[A-Za-z0-9_]/ ) {
            $j += 2; next;
        }
        last;
    }
    return [ 'number', substr( $d, $i, $j - $i ) ], $j;
}

# Scan the longest punctuator at $i.  Returns a (type, text) token and the
# index just past it, or an empty list when no punctuator matches.
sub _scan_punct {
    my ( $d, $i ) = @_;
    for my $p (@PUNCT) {
        if ( substr( $d, $i, length($p) ) eq $p ) {
            return [ 'punct', $p ], $i + length($p);
        }
    }
    return;
}

# True when $tk is the given punctuator.
sub _is_punct {
    my ( $tk, $p ) = @_;
    return defined $tk && $tk->[0] eq 'punct' && $tk->[1] eq $p;
}

# Ignore layout outside directives. Inside directives a logical newline is
# significant, as is adjacency between the name of a function-like macro and
# its opening parenthesis. Comments separate tokens even when they contain no
# spaces; tokenize() records adjacency directly from the translated bytes.
sub _normalize {
    my ($t) = @_;
    my @out;
    my $bol = 1;
    my $directive = 0;
    my $define = 0;
    for my $tk (@$t) {
        next if $tk->[0] eq 'comment';
        if ( $tk->[0] eq 'nl' ) {
            push @out, [ 'directive_end', '' ] if $directive;
            $bol = 1;
            $directive = 0;
            $define = 0;
            next;
        }
        if ( $bol && _is_punct( $tk, '#' ) ) {
            $directive = 1;
            $define = 1;
        }
        elsif ( $define == 1 ) {
            $define = $tk->[0] eq 'ident' && $tk->[1] eq 'define' ? 2 : 0;
        }
        elsif ( $define == 2 ) {
            push @out, [ 'macro_kind', $tk->[2] ? 'function' : 'object' ];
            $define = 0;
        }
        push @out, $tk;
        $bol = 0;
    }
    push @out, [ 'directive_end', '' ] if $directive;
    return \@out;
}

sub _canon {
    my ($toks) = @_;
    my $s = '';
    for my $tk (@$toks) { $s .= $tk->[0] . ':' . $tk->[1] . "\x00"; }
    return $s;
}

sub _comments {
    my ($t) = @_;
    my @c;
    for my $tk (@$t) {
        next unless $tk->[0] eq 'comment';
        my $s = $tk->[1];
        $s =~ s/\s+//g;
        push @c, $s;
    }
    return join( "\x00", sort @c );
}

# Returns nothing (false) when equivalent, else a (category, detail) list.
sub compare {
    my ( $a, $b ) = @_;
    my $ta = tokenize($a);
    my $tb = tokenize($b);

    # An unterminated literal (a raw newline inside it) cannot be
    # interpreted as a preprocessing token; report it as unsupported rather
    # than comparing bytes that have no defined meaning.
    for my $tk ( @$ta, @$tb ) {
        return [ 'ORACLE_UNSUPPORTED', '' ] if $tk->[0] eq 'uliteral';
    }

    my $ca = _canon( _normalize($ta) );
    my $cb = _canon( _normalize($tb) );
    if ( $ca ne $cb ) {
        my @la = split /\x00/, $ca, -1;
        my @lb = split /\x00/, $cb, -1;
        my $cat =
            @la > @lb ? 'TOKEN_LOSS'
          : @la < @lb ? 'TOKEN_DUPLICATION'
          :             'TOKEN_CHANGE';
        return [ $cat, '' ];
    }
    if ( _comments($ta) ne _comments($tb) ) {
        return [ 'COMMENT_CHANGE', '' ];
    }
    return;
}

sub _selftest {
    my @cases = (
        [ "int x;\n",               "int  x ;\n",             1 ],
        [ "int x; int y;\n",        "int x;\nint y;\n",       1 ],
        [ "int f(void){}\n",        "int f(void){}\n",        1 ],
        [ "int x; int y;\n",        "int x;\n",               0 ],
        [ "int x;\n",               "int x; int x;\n",        0 ],
        [ "int x = 1;\n",           "int x = 2;\n",           0 ],
        [ "a + b;\n",               "a - b;\n",               0 ],
        [ "int x; // c\nint y;\n",  "int x; // c int y;\n",   0 ],
        [ "/* a */ int x;\n",       "int x; /* a */\n",       1 ],
        [ "char *s = \"/*x*/\";\n", "char *s = \"/*x*/\";\n", 1 ],
        [
            "#if A\nint a;\n#else\nint b;\n#endif\n",
            "#if A\nint b;\n#else\nint a;\n#endif\n",
            0
        ],
        [ "#define X(a) a ## b\n", "#define X(a) a ## b\n", 1 ],
        [ "#define X(a) a ## b\n", "#define X(a) a # b\n",  0 ],
        [ "int x\\\n;\n",          "int x;\n",              1 ],

        # Trigraph line continuation (??/ + newline is a splice).
        [ "int x??/\n;\n", "int x;\n", 1 ],

        # Identifier concatenation.
        [ "int x; int y;\n", "int x; inty;\n", 0 ],

        # Operator concatenation.
        [ "a + +b;\n",  "a ++b;\n",  0 ],
        [ "a - -b;\n",  "a --b;\n",  0 ],
        [ "a + = b;\n", "a += b;\n", 0 ],

        # Token splitting.
        [ "a++;\n",  "a + +;\n",  0 ],
        [ "a->b;\n", "a - >b;\n", 0 ],

        # Deleted punctuation.
        [ "int f(int a, int b);\n", "int f(int a int b);\n", 0 ],
        [ "int a, b;\n",            "int a b;\n",            0 ],
        [ "int *a, *b;\n",          "int *a *b;\n",          0 ],
        [ "int a; int b;\n",        "int a int b;\n",        0 ],

        # Truncated source at end of file.
        [ "int a; int b;\n", "int a;\n", 0 ],

        # Duplicate punctuation.
        [ "int f(int a);\n", "int f(int a,,);\n", 0 ],

        # Changed literal bytes.
        [ "char c = 'a';\n", "char c = 'b';\n", 0 ],

        # Truncated literal.
        [ "char *s = \"abcd\";\n", "char *s = \"abc\";\n", 0 ],

        # Swallowed comment.
        [ "int x; /* c */ int y;\n", "int x; int y;\n", 0 ],

        # Escaped-newline change alters the spliced token stream.
        [ "a\\\nb;\n", "a b;\n", 0 ],

        # Deleted conditional branch.
        [
            "#if A\nint a;\n#else\nint b;\n#endif\n",
            "#if A\nint a;\n#endif\n", 0
        ],

        # Duplicated conditional branch.
        [
            "#if A\nint a;\n#else\nint b;\n#endif\n",
            "#if A\nint a;\n#else\nint a;\n#else\nint b;\n#endif\n",
            0
        ],

        # Unterminated literal (raw newline) is unsupported.
        [ "char *s = \"a\nb\";\n", "char *s = \"a\nb\";\n", 2 ],

        # Trigraph replacement is a character: a replacement that
        # combines with the next character forms a compound punctuator.
        [ "a ??'= b;\n", "a ??'= b;\n",  1 ],
        [ "a ??'= b;\n", "a ??' = b;\n", 0 ],
        [ "a ??!= b;\n", "a ??!= b;\n",  1 ],
        [ "a ??!| b;\n", "a ??!| b;\n",  1 ],
        [ "a ??!| b;\n", "a ??! | b;\n", 0 ],

        # A trigraph replacement stays a single character next to its
        # neighbours: ??' before a digit is not a comment or literal.
        [ "x = a ??' b;\n", "x = a??'b;\n", 1 ],

        # Identifier split across a backslash-newline splice.
        [ "int ab\\\ncd;\n", "int abcd;\n",  1 ],
        [ "int ab\\\ncd;\n", "int ab cd;\n", 0 ],

        # Backslash-CRLF splice and consecutive splices.
        [ "a\\\r\nb;\n",   "ab;\n", 1 ],
        [ "a\\\n\\\nb;\n", "ab;\n", 1 ],

        # Splice inside a line comment continues the comment onto the
        # next physical line, unlike a real line break.
        [ "// c\\\nx\nint a;\n", "// c\nx\nint a;\n", 0 ],

        # Question marks that are not trigraphs stay punctuators.
        [ "a ? b : c;\n", "a?b:c;\n", 1 ],
    );
    push @cases,
        [ "#define F(x) x\n", "#define F (x) x\n", 0 ],
        [ "#define F(x) x\n", "#define F/**/(x) x\n", 0 ],
        [ "#define X 1\nint x;\n", "#define X 1 int x;\n", 0 ],
        [ "#define X 1", "#define X 1\n", 1 ],
        [ 'u8"x"', 'u8 "x"', 0 ],
        [ "L'x'", "L 'x'", 0 ],
        [ 'u"x"', 'u "x"', 0 ],
        [ 'U"x"', 'U "x"', 0 ],
        [ 'void f(void) { while (x); }', 'void f(void) { while (x) }', 0 ],
        [ 'int x;;', 'int x;', 0 ],
        [ 'int x;', 'int x', 0 ],
        [ "int x;\\", 'int x;', 0 ],
        [ 'double x = 1e+2;', 'double x = 1e + 2;', 0 ],
        [ "int x = 1'000;", "int x = 1 '0' 00;", 0 ];
    my $bad = 0;
    for my $c (@cases) {
        my ( $a, $b, $ok ) = @$c;
        my $r = compare( $a, $b );
        my $pass;
        if ( $ok == 2 ) {
            $pass = defined($r) && $r->[0] eq 'ORACLE_UNSUPPORTED';
        }
        else {
            $pass = $ok ? !defined($r) : defined($r) && $r->[0] ne 'ORACLE_UNSUPPORTED';
        }
        if ( !$pass ) {
            $bad++;
            print "SELFTEST FAIL: $a -> $b\n";
        }
    }
    print "oracle selftest: " . scalar(@cases) . " cases, $bad failures\n";
    return $bad;
}

return 1 if caller;

if ( @ARGV && $ARGV[0] eq '--selftest' ) {
    exit( _selftest() ? 1 : 0 );
}
if ( @ARGV == 3 && ( $ARGV[0] eq 'compare' || $ARGV[0] eq 'compare-known' ) ) {
    open( my $fa, '<:raw', $ARGV[1] ) or die "$ARGV[1]: $!\n";
    local $/ = undef;
    my $a = <$fa>;
    close $fa                         or die "$ARGV[1]: $!\n";
    open( my $fb, '<:raw', $ARGV[2] ) or die "$ARGV[2]: $!\n";
    my $b = <$fb>;
    close $fb or die "$ARGV[2]: $!\n";
    my $r = compare( $a, $b );
    if ( $ARGV[0] eq 'compare-known' ) {
        open( my $known, '<', "$Bin/semantic-known.tsv" ) or die "semantic-known.tsv: $!\n";
        my $name = $ARGV[1];
        $name =~ s{.*/}{};
        local $/ = "\n";
        while ( my $line = <$known> ) {
            next if $line =~ /\A#/ || $line =~ /\A\s*\z/;
            chomp $line;
            my ( $file, $input, $output, $reason ) = split /\t/, $line, 4;
            next unless $file eq $name;
            if ( sha256_hex($a) eq $input && sha256_hex($b) eq $output && defined $r ) {
                print "KNOWN_EXCEPTION: $file: $reason\n";
                exit 0;
            }
            die "known semantic exception changed or fixed: $file; review manifest\n";
        }
        close $known or die "semantic-known.tsv: $!\n";
    }
    exit 0 unless defined $r;
    print "$r->[0]\n";
    exit 1;
}
die "usage: semanticoracle.pl --selftest | compare A B | compare-known A B\n";
