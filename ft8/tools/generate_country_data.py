#!/usr/bin/env python3
"""Generate the browser's offline country table from an AD1C CTY.DAT file.

Usage: python3 ft8/tools/generate_country_data.py /path/to/cty.dat
Source: https://www.country-files.com/cty/cty.dat
Format: https://www.country-files.com/cty-dat-format/
"""
import argparse
import hashlib
import json
from pathlib import Path
import re


# Short German labels for countries; island/entity names otherwise follow CTY.
NAMES = {
    'Sov Mil Order of Malta': 'Malteserorden', 'Spratly Islands': 'Spratly-Inseln',
    'Equatorial Guinea': 'Äquatorialguinea', 'Kingdom of Eswatini': 'Eswatini',
    'Tunisia': 'Tunesien', 'Azerbaijan': 'Aserbaidschan', 'Georgia': 'Georgien',
    'Timor - Leste': 'Osttimor', 'Libya': 'Libyen', 'Cyprus': 'Zypern',
    'Tanzania': 'Tansania', 'Mauritania': 'Mauretanien', 'Jamaica': 'Jamaika',
    'Yemen': 'Jemen', 'Algeria': 'Algerien', 'Maldives': 'Malediven',
    'Croatia': 'Kroatien', 'Zambia': 'Sambia', 'West Malaysia': 'Westmalaysia',
    'East Malaysia': 'Ostmalaysia', 'Dem. Rep. of the Congo': 'DR Kongo',
    'Singapore': 'Singapur', 'Rwanda': 'Ruanda', 'United Arab Emirates': 'VAE',
    'Qatar': 'Katar', 'Taiwan': 'Taiwan', 'China': 'China', 'The Gambia': 'Gambia',
    'Mozambique': 'Mosambik', 'Easter Island': 'Osterinsel', 'Antarctica': 'Antarktis',
    'Cuba': 'Kuba', 'Morocco': 'Marokko', 'Bolivia': 'Bolivien', 'Azores': 'Azoren',
    'Cabo Verde': 'Kap Verde', 'Comoros': 'Komoren', 'Fed. Rep. of Germany': 'Deutschland',
    'Philippines': 'Philippinen', 'Palestine': 'Palästina',
    'Bosnia-Herzegovina': 'Bosnien-Herzegowina', 'Spain': 'Spanien',
    'Balearic Islands': 'Balearen', 'Canary Islands': 'Kanaren',
    'Ireland': 'Irland', 'Armenia': 'Armenien', 'Moldova': 'Moldau',
    'Estonia': 'Estland', 'Ethiopia': 'Äthiopien', 'Belarus': 'Belarus',
    'Kyrgyzstan': 'Kirgisistan', 'Tajikistan': 'Tadschikistan',
    'France': 'Frankreich', 'New Caledonia': 'Neukaledonien',
    'French Polynesia': 'Franz.-Polynesien', 'French Guiana': 'Franz.-Guayana',
    'Northern Ireland': 'Nordirland', 'Scotland': 'Schottland',
    'Solomon Islands': 'Salomonen', 'Hungary': 'Ungarn', 'Switzerland': 'Schweiz',
    'Dominican Republic': 'Dominikanische Rep.', 'Colombia': 'Kolumbien',
    'Republic of Korea': 'Südkorea', 'Vatican City': 'Vatikan',
    'Saudi Arabia': 'Saudi-Arabien', 'Italy': 'Italien', 'Sardinia': 'Sardinien',
    'Djibouti': 'Dschibuti', 'Mongolia': 'Mongolei', 'Svalbard': 'Spitzbergen',
    'Bear Island': 'Bäreninsel', 'Jordan': 'Jordanien', 'United States': 'USA',
    'American Samoa': 'Amerikanisch-Samoa', 'US Virgin Islands': 'US-Jungferninseln',
    'Norway': 'Norwegen', 'Argentina': 'Argentinien', 'Luxembourg': 'Luxemburg',
    'Lithuania': 'Litauen', 'Bulgaria': 'Bulgarien', 'Lebanon': 'Libanon',
    'Austria': 'Österreich', 'Finland': 'Finnland', 'Aland Islands': 'Åland',
    'Czech Republic': 'Tschechien', 'Slovak Republic': 'Slowakei',
    'Belgium': 'Belgien', 'Greenland': 'Grönland', 'Faroe Islands': 'Färöer',
    'Denmark': 'Dänemark', 'Papua New Guinea': 'Papua-Neuguinea',
    'DPR of Korea': 'Nordkorea', 'Netherlands': 'Niederlande', 'Brazil': 'Brasilien',
    'Western Sahara': 'Westsahara', 'Bangladesh': 'Bangladesch', 'Slovenia': 'Slowenien',
    'Seychelles': 'Seychellen', 'Sweden': 'Schweden', 'Poland': 'Polen',
    'Egypt': 'Ägypten', 'Greece': 'Griechenland', 'Mount Athos': 'Athos',
    'Dodecanese': 'Dodekanes', 'Crete': 'Kreta', 'Western Kiribati': 'Westkiribati',
    'Central Kiribati': 'Zentralkiribati', 'Eastern Kiribati': 'Ostkiribati',
    'Asiatic Turkey': 'Türkei', 'Iceland': 'Island', 'Cameroon': 'Kamerun',
    'Corsica': 'Korsika', 'Central African Republic': 'Zentralafrikanische Rep.',
    'Republic of the Congo': 'Republik Kongo', 'Chad': 'Tschad',
    "Cote d'Ivoire": 'Elfenbeinküste', 'European Russia': 'Russland (EU)',
    'Asiatic Russia': 'Russland (AS)', 'Uzbekistan': 'Usbekistan',
    'Kazakhstan': 'Kasachstan', 'Brunei Darussalam': 'Brunei', 'Canada': 'Kanada',
    'Australia': 'Australien', 'Christmas Island': 'Weihnachtsinsel',
    'British Virgin Islands': 'Brit. Jungferninseln', 'India': 'Indien',
    'Mexico': 'Mexiko', 'Cambodia': 'Kambodscha', 'Macao': 'Macau',
    'Indonesia': 'Indonesien', 'Iraq': 'Irak', 'Syria': 'Syrien',
    'Latvia': 'Lettland', 'Romania': 'Rumänien', 'Serbia': 'Serbien',
    'Zimbabwe': 'Simbabwe', 'North Macedonia': 'Nordmazedonien',
    'Republic of Kosovo': 'Kosovo', 'Republic of South Sudan': 'Südsudan',
    'Albania': 'Albanien', 'UK Base Areas on Cyprus': 'Brit. Basen auf Zypern',
    'New Zealand': 'Neuseeland', 'South Africa': 'Südafrika',
}

NOTICE = '''/*
Country prefix data: Copyright (c) 1994- Jim Reisert, AD1C.
Source and license: https://www.country-files.com/copyright/

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
*/
'''


def generate(raw):
    text = raw.decode('utf-8-sig')
    version = re.search(r'=VER(\d{8})(?:[,;\s]|$)', text)
    if not version:
        raise ValueError('Missing CTY version marker')
    entities = []
    for record in text.split(';'):
        if not record.strip():
            continue
        fields = record.strip().split(':', 8)
        if len(fields) != 9:
            raise ValueError('Invalid CTY country header')
        country, primary, aliases = fields[0].strip(), fields[7].strip(), fields[8]
        if primary.startswith('*'):  # WAE-only entities are not DXCC countries.
            continue
        keys = []
        for alias in aliases.split(','):
            # Drop CQ/ITU-zone, continent, coordinates and time-zone modifiers.
            key = re.split(r'[([<{~]', alias.strip(), maxsplit=1)[0]
            if not re.fullmatch(r'=?[A-Z0-9]+(?:/[A-Z0-9]+)*', key):
                raise ValueError(f'Invalid CTY alias: {alias!r}')
            if key not in ('=VERSION', '=VER' + version[1]):
                keys.append(key)
        entities.append([NAMES.get(country, country), ','.join(keys)])
    if len(entities) < 300:
        raise ValueError('Incomplete country table')
    data = {'version': version[1], 'entities': entities}
    rows = ',\n'.join('    ' + json.dumps(row, ensure_ascii=False) for row in entities)
    header = (f'// Generated from https://www.country-files.com/cty/cty.dat; CTY {version[1]}.\n'
              f'// Source SHA-256: {hashlib.sha256(raw).hexdigest()}\n'
              '// Regenerate with ft8/tools/generate_country_data.py; do not edit.\n')
    return NOTICE + header + 'globalThis.FT8CountryData = {\n' + \
        f'    version: "{data["version"]}",\n    entities: [\n{rows}\n    ]\n}};\n'


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('cty_file', type=Path)
    parser.add_argument('--output', type=Path,
                        default=Path(__file__).resolve().parents[2] / 'html/ft8-country-data.js')
    args = parser.parse_args()
    result = generate(args.cty_file.read_bytes())
    args.output.write_text(result, encoding='utf-8')
    print(f'Generated {args.output} ({len(result.encode())} bytes)')
