#!/usr/bin/env python3
"""XMLTV guide for the demo line-up: made-up shows around the current time."""
import datetime as dt
import random
from xml.sax.saxutils import escape

SHOWS = {
    "fractal.demo": [("The Infinite Zoom", "A camera falls forever into the edge of a fractal. Tonight: the seahorse valley."),
                     ("Midnight Matinee", "A double bill of slow, swirling colour from the vaults."),
                     ("Edge of the Set", "Classic adventure in the complex plane, digitally remastered.")],
    "life.demo": [("Colony", "Cells are born, live and die by four simple rules. Narrated in silence."),
                  ("Gliders Over Africa", "Small patterns migrate across a vast grid."),
                  ("The Oscillator Files", "A look inside the blinkers, toads and beacons of the grid.")],
    "gradient.demo": [("Colour Club", "Bright shapes and friendly shades for younger viewers."),
                      ("Rainbow Road Trip", "Two gradients cross the country in a borrowed camper van."),
                      ("Saturday Toons", "Back-to-back cartoons in glorious widescreen.")],
    "pattern.demo": [("Evening News", "Tonight's top stories, weather and sport."),
                     ("Newsnight Extra", "Analysis and debate on the day's events."),
                     ("Morning Report", "Wake up with the headlines.")],
    "rule110.demo": [("Chart Show", "This week's biggest hits, counted down."),
                     ("Late Lounge", "Smooth sounds for the small hours."),
                     ("Live from the Grid", "Rule 110 performs its greatest hits, cell by cell.")],
    "weather.demo": [("Weather Radar", "Rolling forecast for your region.")],
    "late.demo": [("Late Night Movie", "A classic film to end the day.")],
}
NAMES = {"fractal.demo": "Fractal Movies", "life.demo": "Life Science", "gradient.demo": "Gradient Kids",
         "pattern.demo": "Pattern News", "rule110.demo": "Rule 110 Music", "weather.demo": "Weather Radar",
         "late.demo": "Late Night"}

def stamp(t):
    return t.strftime("%Y%m%d%H%M%S +0000")

now = dt.datetime.now(dt.timezone.utc).replace(second=0, microsecond=0)
start0 = now.replace(minute=0) - dt.timedelta(hours=6)
rnd = random.Random(7)
out = ['<?xml version="1.0" encoding="UTF-8"?>', '<tv generator-info-name="retrotv-demo">']
for cid, name in NAMES.items():
    out.append(f'  <channel id="{cid}"><display-name>{escape(name)}</display-name></channel>')
for cid, shows in SHOWS.items():
    t, ep = start0 + dt.timedelta(minutes=rnd.choice([0, 15])), 1
    while t < now + dt.timedelta(hours=24):
        title, desc = shows[rnd.randrange(len(shows))]
        length = dt.timedelta(minutes=rnd.choice([25, 30, 45, 60, 90]))
        out.append(f'  <programme start="{stamp(t)}" stop="{stamp(t + length)}" channel="{cid}">')
        out.append(f'    <title>{escape(title)}</title><sub-title>Episode {ep}</sub-title>')
        out.append(f'    <desc>{escape(desc)}</desc>')
        out.append('  </programme>')
        t, ep = t + length, ep + 1
out.append('</tv>')
print("\n".join(out))
