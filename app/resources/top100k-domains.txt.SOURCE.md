# top100k-domains.txt

The top 100,000 rows (by `GlobalRank`) of the [Majestic Million](https://majestic.com/reports/majestic-million),
a freely downloadable, publicly available ranked list of domains by
referring-subnet count, fetched from https://downloads.majestic.com/majestic_million.csv.
One domain per line, most popular first.

A couple of Ohm-curated entries sit next to `github.com` (the repo itself,
and omarchy.org) so they autocomplete even though they aren't in Majestic's
top 100k as written.

Used entirely offline for the omnibox's autocomplete (`app/src/PopularDomains.cpp`) --
no network request is ever made for it, and it contains no information about
this browser's own users.
