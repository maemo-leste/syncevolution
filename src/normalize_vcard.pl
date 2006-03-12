#! /usr/bin/perl -w

use strict;

$_ = join( "", grep( !/^(BEGIN:VCARD|VERSION|END:VCARD|UID:)/, <> ) );
s/\r//g;

my @cards = ();

foreach $_ ( split( /\n\n/ ) ) {
  # undo line continuation
  s/\n\s//gs;
  # ignore charset specifications, assume UTF-8
  s/;CHARSET="UTF-8"//g;
  # ignore extra email type
  s/EMAIL;TYPE=INTERNET/EMAIL/g;
  # ignore extra ADR type
  s/ADR;TYPE=OTHER/ADR/g;
  # the type of certain fields is ignore by Evolution
  s/X-(AIM|GROUPWISE|ICQ|YAHOO);TYPE=HOME/X-$1/g;
  # sort entries, putting "N:" first
  my @lines = split( "\n" );
  push @cards, join( "\n", grep( /^N:/, @lines ), sort( grep ( !/^N:/, @lines ) ) );
}

print join( "\n\n", sort @cards ), "\n";