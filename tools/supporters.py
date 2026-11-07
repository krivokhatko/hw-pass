#!/usr/bin/env python3
"""Update the supporter list of README.md and the badge data from Donatello.

Usage:
    DONATELLO_TOKEN=<token> tools/supporters.py

The token comes from the API settings of the page (donatello.to/panel). The
CI keeps it in the secret DONATELLO_TOKEN (.github/workflows/supporters.yml).

Reads:  https://donatello.to/api/v1/me       the number of donations
        https://donatello.to/api/v1/donates  the donations, with the names
Writes: docs/media/supporters.json           the data of the shields.io badge
        README.md                            the block between the markers
                                             <!-- supporters:start --> and
                                             <!-- supporters:end -->

Python 3 only, no other tool. The output is the same for the same data, so a
run without a new donation changes no file.
"""
import json
import os
import sys
import urllib.request
from pathlib import Path

API = 'https://donatello.to/api/v1'
ROOT = Path(__file__).resolve().parent.parent
README = ROOT / 'README.md'
BADGE = ROOT / 'docs' / 'media' / 'supporters.json'
START = '<!-- supporters:start -->'
END = '<!-- supporters:end -->'
TOP = 20          # names in the list, the largest sums first
NAME_MAX = 40     # characters of a name


def get(path, token):
    req = urllib.request.Request(API + path, headers={'X-Token': token, 'User-Agent': 'hw-pass-supporters'})
    with urllib.request.urlopen(req, timeout=30) as r:
        return json.load(r)


def donates(token):
    """All donations, newest first. The API gives pages of at most 100."""
    page = 0
    while True:
        d = get(f'/donates?page={page}&size=100', token)
        yield from d.get('content') or []
        if d.get('last', True) or page >= 50:
            return
        page += 1


def escape(name):
    """A name as plain text in Markdown: no emphasis, no links, no HTML."""
    name = ' '.join(name.split())[:NAME_MAX]
    return ''.join('\\' + c if c in '\\`*_{}[]()<>#+-!|~' else c for c in name)


def main():
    token = os.environ.get('DONATELLO_TOKEN')
    if not token:
        sys.exit('supporters.py: DONATELLO_TOKEN is not set')

    me = get('/me', token)
    count = int((me.get('donates') or {}).get('totalCount') or 0)

    totals = {}
    for d in donates(token):
        if d.get('isPublished') is False:
            continue
        name = (d.get('clientName') or '').strip()
        if not name:
            continue
        totals[name] = totals.get(name, 0.0) + float(d.get('amount') or 0)
    names = sorted(totals, key=lambda n: (-totals[n], n.lower()))[:TOP]

    badge = {'schemaVersion': 1, 'label': 'supporters', 'message': str(count), 'color': 'ffb000'}
    BADGE.write_text(json.dumps(badge) + '\n', encoding='utf-8')

    text = README.read_text(encoding='utf-8')
    a = text.index(START) + len(START)
    b = text.index(END)
    line = ''
    if names:
        line = 'Thank you to the people who bought a coffee: ' + ', '.join(escape(n) for n in names) + '.\n'
    README.write_text(text[:a] + '\n' + line + text[b:], encoding='utf-8')

    print(f'supporters.py: {count} donations, {len(names)} names in README.md')


if __name__ == '__main__':
    main()
