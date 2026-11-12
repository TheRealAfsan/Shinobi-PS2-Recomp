"""Builds the launcher page from the Claude Design file.

usage: python build_launcher_page.py [design.html] [box scan .jpg] [out.html]

Takes the design's bundled page as it is (styles, fonts, images, the Component logic) and changes only what is needed for
it to drive the real game through the WebView2 host (ShinobiLauncher.exe):
  - the page fills the launcher window (the design shows it on a 1100x720 stage);
  - title bar: drag, minimise, close go to the host;
  - settings, key bindings and mods load from / save to the host (game/settings.ini, launcher.ini);
  - Play starts the real game, Stop Game ends it, the status chip shows the real state;
  - options this port has: renderer D3D11/Vulkan, Edge AA instead of MSAA, widescreen 16:9/21:9/32:9/Auto chips, mods
    that exist (lock to 30 fps, fast VU maths, fullscreen, record diagnostics, legacy sound chip);
  - the four guide pictures (cropped from the box scan's back cover) are packed in, and the credits name PCSX2.
Rerun it after changing the design.
"""
import base64
import io
import json
import os
import re
import sys
import uuid

from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
design = sys.argv[1] if len(sys.argv) > 1 else os.path.join(r'X:\HAPPYNESS\shinobi_port\Launcher Claude Design',
                                                             'Shinobi PS2 Recom Launcher \u2013 Launcher.html')
box_scan = sys.argv[2] if len(sys.argv) > 2 else os.path.join(os.environ['USERPROFILE'], 'Pictures', 'Shinobi - Box.jpg')
out_path = sys.argv[3] if len(sys.argv) > 3 else os.path.join(ROOT, 'launcher_assets', 'launcher.html')

page = open(design, encoding='utf-8').read()


def get_block(name):
    m = re.search(r'(<script type="__bundler/%s">\s*)(.*?)(\s*</script>)' % re.escape(name), page, re.S)
    if not m:
        raise SystemExit('design file has no __bundler/%s block' % name)
    return m


def set_block(name, text):
    global page
    m = get_block(name)
    page = page[:m.start(2)] + text + page[m.end(2):]


manifest = json.loads(get_block('manifest').group(2))
template = json.loads(get_block('template').group(2))

# The SEGA and PlayStation logo pictures are not used (always): their names stay on the page as plain text.
_b = io.BytesIO()
Image.new('RGBA', (2, 2), (0, 0, 0, 0)).save(_b, 'PNG')
_blank_logo = base64.b64encode(_b.getvalue()).decode()
for _uid in ('2e2c0ffd-b51c-4997-b617-7a8681613443', 'a6a19fc5-2af0-4019-98aa-627e71c90e00'):
    if _uid in manifest:
        manifest[_uid] = {'mime': 'image/png', 'compressed': False, 'data': _blank_logo}
template = re.sub(r'<img src="[^"]+" alt="PlayStation" height="30">\s*<img src="[^"]+" alt="SEGA" height="22">',
                  '<span class="mark">Shinobi &copy; SEGA<br>PlayStation is a trademark of Sony</span>', template, count=1)
assert template.count('@media (prefers-reduced-motion:reduce)') == 1
template = template.replace('@media (prefers-reduced-motion:reduce)',
                            '.mark{font-family:Rajdhani,sans-serif;font-size:11px;font-weight:700;line-height:1.25;color:#3b3b42;letter-spacing:.02em}'
                            '@media (prefers-reduced-motion:reduce)')

# SHINOBI_PUBLIC_PAGE=1: the page for the public release. The key art, the game's title logo, the SEGA and PlayStation logos,
# the four guide pictures (cut from the box) and the game's back-of-box text are replaced by plain pictures and text made here.
PUBLIC = os.environ.get('SHINOBI_PUBLIC_PAGE') == '1'


def _font(size):
    from PIL import ImageFont
    for name in ('impact.ttf', 'arialbd.ttf', 'segoeuib.ttf', 'arial.ttf'):
        try:
            return ImageFont.truetype(os.path.join(os.environ.get('WINDIR', r'C:\Windows'), 'Fonts', name), size)
        except OSError:
            continue
    return ImageFont.load_default()


def _png(img):
    buf = io.BytesIO()
    img.save(buf, 'PNG', optimize=True)
    return buf.getvalue()


def _jpg(img):
    buf = io.BytesIO()
    img.convert('RGB').save(buf, 'JPEG', quality=88)
    return buf.getvalue()


def _gradient(w, h, top, bottom):
    from PIL import ImageDraw
    img = Image.new('RGB', (w, h), top)
    d = ImageDraw.Draw(img)
    for y in range(h):
        k = y / max(h - 1, 1)
        d.line([(0, y), (w, y)], fill=tuple(int(top[i] + (bottom[i] - top[i]) * k) for i in range(3)))
    return img


def _public_assets():
    from PIL import ImageDraw
    out = {}
    # hero: dark crimson, a few soft diagonal bands
    hero = _gradient(1200, 1500, (74, 12, 30), (12, 10, 16))
    d = ImageDraw.Draw(hero, 'RGBA')
    for i, a in enumerate((26, 18, 12)):
        x = 140 + i * 330
        d.polygon([(x, 0), (x + 150, 0), (x - 380, 1500), (x - 530, 1500)], fill=(179, 18, 42, a))
    out['10424391-f9dc-401f-80f1-60c74c105824'] = ('image/jpeg', _jpg(hero))
    # logo: plain text title (transparent png)
    logo = Image.new('RGBA', (900, 300), (0, 0, 0, 0))
    d = ImageDraw.Draw(logo)
    f1, f2 = _font(190), _font(58)
    d.text((450, 120), 'SHINOBI', font=f1, fill=(244, 244, 246, 255), anchor='mm', stroke_width=7, stroke_fill=(179, 18, 42, 255))
    d.text((450, 262), 'PS2  RECOMP', font=f2, fill=(244, 244, 246, 255), anchor='mm', stroke_width=3, stroke_fill=(20, 12, 16, 255))
    out['0fc8ae0c-9e41-4676-90d9-741760077d0e'] = ('image/png', _png(logo))
    # the SEGA and PlayStation logos: empty
    blank = _png(Image.new('RGBA', (2, 2), (0, 0, 0, 0)))
    out['2e2c0ffd-b51c-4997-b617-7a8681613443'] = ('image/png', blank)
    out['a6a19fc5-2af0-4019-98aa-627e71c90e00'] = ('image/png', blank)
    return out


if PUBLIC:
    for uid, (mime, data) in _public_assets().items():
        assert uid in manifest, uid
        manifest[uid] = {'mime': mime, 'compressed': False, 'data': base64.b64encode(data).decode()}
    template = template.replace('alt="Shinobi key art: Hotsuma with sword raised, flanked by two women"', 'alt="Shinobi PS2 Recomp"')


def must_replace(text, old, new, count=1):
    n = text.count(old)
    if n != count:
        raise SystemExit('design changed? expected %d x %r, found %d' % (count, old[:70], n))
    return text.replace(old, new)


# ---------------------------------------------------------------- guide pictures (back of the box)
GUIDES = [  # (key, caption, label, crop in back-cover thumbnail units (280 x 420 for the left 45% of the scan))
    ('chain', 'Chain attacks together to destroy hordes of enemies', 'Chain attacks', (24, 104, 126, 178)),
    ('magic', 'Unleash devastating Ninja Magic', 'Ninja Magic', (145, 140, 247, 213)),
    ('wall', 'Wall run to explore secret areas', 'Wall run', (17, 209, 119, 283)),
    ('dash', 'Stealth-dash to confuse enemies with ghost images', 'Stealth dash', (137, 236, 238, 310)),
]
if PUBLIC:  # the guides' captions in plain words of our own (the box's phrases are not used)
    GUIDES = [('chain', 'String sword attacks together to deal with groups of enemies', 'Chain attacks', None),
              ('magic', 'Ninja Magic is a powerful special attack', 'Ninja Magic', None),
              ('wall', 'Run along walls to reach hidden places', 'Wall run', None),
              ('dash', 'Dash through enemies to get out of their way', 'Stealth dash', None)]
guide_ids = {}
if PUBLIC:
    from PIL import ImageDraw
    for key, caption, label, _ in GUIDES:
        card = _gradient(900, 500, (58, 14, 28), (14, 12, 18))
        d = ImageDraw.Draw(card)
        d.rectangle([0, 0, 899, 499], outline=(179, 18, 42), width=6)
        d.text((450, 250), label.upper(), font=_font(84), fill=(244, 244, 246), anchor='mm')
        uid = str(uuid.uuid5(uuid.NAMESPACE_URL, 'shinobi-launcher-guide-' + key))
        manifest[uid] = {'mime': 'image/jpeg', 'compressed': False, 'data': base64.b64encode(_jpg(card)).decode()}
        guide_ids[key] = uid
elif os.path.exists(box_scan):
    scan = Image.open(box_scan).convert('RGB')
    scale = (0.45 * scan.width) / 280.0
    for key, _, _, box in GUIDES:
        crop = scan.crop(tuple(int(v * scale) for v in box))
        crop.thumbnail((900, 600))
        buf = io.BytesIO()
        crop.save(buf, 'JPEG', quality=88)
        uid = str(uuid.uuid5(uuid.NAMESPACE_URL, 'shinobi-launcher-guide-' + key))
        manifest[uid] = {'mime': 'image/jpeg', 'compressed': False, 'data': base64.b64encode(buf.getvalue()).decode()}
        guide_ids[key] = uid
else:
    print('warning: box scan not found, guide pictures stay empty:', box_scan)

# ---------------------------------------------------------------- styles: fill the launcher window
css = """
html{width:100%;height:100%;overflow:hidden;background:#16161a;display:grid;place-items:center}
body{margin:0;width:1000px;height:640px;overflow:hidden;background:#16161a}
.stage{width:1000px!important;height:640px!important;background:none!important}
.win{border:0!important;border-radius:0!important;box-shadow:none!important}
.tb{user-select:none;-webkit-user-select:none;app-region:drag;-webkit-app-region:drag}
.tbb{app-region:no-drag;-webkit-app-region:no-drag}
html.manual-open .tb{app-region:no-drag;-webkit-app-region:no-drag}
.foot-wrap{margin-top:auto;display:flex;flex-direction:column;gap:5px}
.foot-wrap .foot{margin-top:0;padding-top:0}
.own{margin:0;font-family:Rajdhani,sans-serif;font-size:11px;font-weight:600;line-height:1.25;color:#4a4a52;
     border-left:2px solid #b3122a;padding:2px 0 2px 7px}
.studio{margin:0;font-family:Rajdhani,sans-serif;font-size:11px;font-weight:600;line-height:1.2;color:#3b3b42;letter-spacing:.02em}
.studio b{color:#17171a;font-weight:700}
.left .btns{gap:7px}
.left .btns .btn{height:38px}
.foot-wrap{gap:4px!important}
.grp{gap:9px!important}
.inl{display:flex;align-items:center;justify-content:space-between;gap:8px}
.inl .fl{margin:0;white-space:nowrap}
.inl .segs{flex:0 0 148px}
.inl .seg{height:30px;font-size:14px;padding:0 2px}
.inl .seg.wide{flex:1.9}
html.ingame .tbb:not(.cl){display:none}
html.ingame .tb{app-region:no-drag;-webkit-app-region:no-drag}
.btn.warn .lab{color:#b3122a}
.pad-focus{outline:3px solid #b3122a!important;outline-offset:2px!important;border-radius:4px}
input[type=range].pad-focus{outline-offset:4px!important}
.ic-mute{display:none}
html.muted .ic-snd{display:none}
html.muted .ic-mute{display:inline}
html.nomusic .snd{opacity:.35}
.ic-res{display:none}
html.maxed .ic-max{display:none}
html.maxed .ic-res{display:inline}
.gview img{height:270px!important}
"""

# ---------------------------------------------------------------- entrance animation
# The key art first (up from black, out of a blur, settling from a slight zoom), then the title bar, the logo (popping in),
# the left column item by item from the left, the settings panel row by row from the right, the footer last.
# --t scales every time: 1 in the launcher, 0.4 in the in-game menu. Off with Windows' "reduce animations".
# Individual transform properties (translate / scale) are animated so the elements' own transforms are kept.
intro_css = """
:root{--t:1.6}
html.ingame{--t:.5}
@keyframes ivHero{from{opacity:0;filter:brightness(.1) blur(10px)}to{opacity:1;filter:none}}
@keyframes ivArt{from{scale:1.14}to{scale:1}}
@keyframes ivLogo{0%{opacity:0;scale:.55;translate:0 22px;filter:blur(8px) drop-shadow(0 4px 10px rgba(40,0,30,.45))}
  62%{opacity:1;scale:1.07;translate:0 -5px;filter:blur(0) drop-shadow(0 4px 10px rgba(40,0,30,.45))}
  100%{opacity:1;scale:1;translate:0 0;filter:drop-shadow(0 4px 10px rgba(40,0,30,.45))}}
@keyframes ivDown{from{opacity:0;translate:0 -100%}to{opacity:1;translate:0 0}}
@keyframes ivFade{from{opacity:0}to{opacity:1}}
@keyframes ivLeft{from{opacity:0;translate:-26px 0}to{opacity:1;translate:0 0}}
@keyframes ivRight{from{opacity:0;translate:26px 0}to{opacity:1;translate:0 0}}
@keyframes ivPop{0%{opacity:0;scale:.92}70%{opacity:1;scale:1.02}100%{opacity:1;scale:1}}
@media (prefers-reduced-motion:no-preference){
.hero{animation:ivHero calc(1.6s*var(--t)) ease-out backwards}
.art{animation:drift 38s ease-in-out infinite alternate,ivArt calc(2.6s*var(--t)) cubic-bezier(.2,.7,.2,1) backwards}
.logo{animation:ivLogo calc(1.1s*var(--t)) cubic-bezier(.2,.9,.3,1) calc(.9s*var(--t)) backwards}
.tb{animation:ivDown calc(.6s*var(--t)) ease-out calc(.3s*var(--t)) backwards}
.left,.right{animation:ivFade calc(.8s*var(--t)) ease-out calc(.6s*var(--t)) backwards}
.grp{animation:ivPop calc(.7s*var(--t)) ease-out calc(1.2s*var(--t)) backwards}
"""
_stagger = []
for k in range(1, 9):  # left column: guides, card (the buttons and footer have their own steps)
    _stagger.append('.left>:not(.btns):not(.foot-wrap):nth-child(%d){animation:ivLeft calc(.55s*var(--t)) ease-out '
                    'calc(%.2fs*var(--t)) backwards}' % (k, 1.0 + 0.12 * k))
for k in range(1, 9):  # left buttons and the game-file caption (every child of .btns)
    _stagger.append('.btns>:nth-child(%d){animation:ivLeft calc(.5s*var(--t)) cubic-bezier(.2,.8,.3,1.15) '
                    'calc(%.2fs*var(--t)) backwards}' % (k, 1.35 + 0.11 * k))
for k in range(1, 13):  # settings rows
    _stagger.append('.grp>:nth-child(%d){animation:ivRight calc(.5s*var(--t)) ease-out calc(%.2fs*var(--t)) backwards}'
                    % (k, 1.35 + 0.075 * k))
for k in range(1, 6):  # footer: notice, logos, studio lines
    _stagger.append('.foot-wrap>:nth-child(%d){animation:ivFade calc(.6s*var(--t)) ease-out calc(%.2fs*var(--t)) backwards}'
                    % (k, 1.85 + 0.1 * k))
# Interactions: the drop-down slides open, buttons slide and light up, switches and tabs change colour smoothly, dialogs
# rise, the toast slides up.
intro_css += '\n'.join(_stagger) + """
@keyframes ivMenu{from{opacity:0;translate:0 -6px;scale:1 .94}to{opacity:1;translate:0 0;scale:1 1}}
@keyframes ivToast{from{opacity:0;translate:0 12px}to{opacity:1;translate:0 0}}
.menu{transform-origin:top center;animation:ivMenu .3s cubic-bezier(.2,.8,.3,1)}
.opt{transition:background .2s,color .2s,padding-left .2s}
.opt:hover{padding-left:14px}
.btn{transition:filter .22s,translate .26s cubic-bezier(.2,.8,.3,1),box-shadow .26s,transform .1s}
.btns .btn:hover{translate:4px 0}
.seg,.chipb,.gtab,.th{transition:background .26s,color .26s,box-shadow .26s,filter .26s}
.th:hover,.gtab:hover{filter:brightness(1.12)}
.chk .bx{transition:background .25s,box-shadow .25s}
.dlg{animation:rise .4s cubic-bezier(.2,.8,.3,1)}
.toast{animation:ivToast .4s ease-out!important}
}
"""
css += intro_css
template = must_replace(template, '@media (prefers-reduced-motion:reduce)', css + '@media (prefers-reduced-motion:reduce)')

# ---------------------------------------------------------------- markup
# The title bar is a real Windows title bar (CSS app-region, see the host): drag, double-click to maximise, snap.
# Left of minimise: the music mute button (speaker / crossed speaker, html.muted picks the icon).
template = must_replace(template, '<button class="tbb" aria-label="Minimize">',
                        '<button class="tbb snd" aria-label="Mute music" title="Music on / off" sc-camel-on-click="{{toggleMute}}">'
                        '<svg class="ic-snd" width="14" height="12" sc-camel-view-box="0 0 14 12" aria-hidden="true">'
                        '<path d="M1 4h3l4-3v10l-4-3H1z" fill="currentColor"></path>'
                        '<path d="M10 3.5c1 .7 1.5 1.5 1.5 2.5s-.5 1.8-1.5 2.5M11.5 1.5c1.6 1.2 2.2 2.8 2.2 4.5s-.6 3.3-2.2 4.5" '
                        'fill="none" stroke="currentColor" stroke-width="1.1"></path></svg>'
                        '<svg class="ic-mute" width="14" height="12" sc-camel-view-box="0 0 14 12" aria-hidden="true">'
                        '<path d="M1 4h3l4-3v10l-4-3H1z" fill="currentColor"></path>'
                        '<path d="M9.5 4l4 4M13.5 4l-4 4" fill="none" stroke="currentColor" stroke-width="1.2"></path></svg>'
                        '</button>'
                        '<button class="tbb" aria-label="Minimize" sc-camel-on-click="{{winMin}}">')
template = must_replace(template,
                        '<button class="tbb" aria-label="Maximize"><svg width="10" height="10" sc-camel-view-box="0 0 10 10" '
                        'aria-hidden="true"><rect x=".5" y=".5" width="9" height="9" fill="none" stroke="currentColor" '
                        'stroke-width="1"></rect></svg></button>',
                        '<button class="tbb" aria-label="Maximize" sc-camel-on-click="{{winMax}}"><svg class="ic-max" width="10" '
                        'height="10" sc-camel-view-box="0 0 10 10" aria-hidden="true"><rect x=".5" y=".5" width="9" height="9" '
                        'fill="none" stroke="currentColor" stroke-width="1"></rect></svg><svg class="ic-res" width="10" height="10" '
                        'sc-camel-view-box="0 0 10 10" aria-hidden="true"><rect x=".5" y="2.5" width="7" height="7" fill="none" '
                        'stroke="currentColor" stroke-width="1"></rect><path d="M2.5 2.5V.5h7v7h-2" fill="none" '
                        'stroke="currentColor" stroke-width="1"></path></svg></button>')
template = must_replace(template, '<button class="tbb cl" aria-label="Close">',
                        '<button class="tbb cl" aria-label="Close" sc-camel-on-click="{{winClose}}">')
# In-game mode (the same page as the game's pause menu, ps2_ingame_menu.cpp): title, card, last left button and the reset
# button take their text from the logic so the game can relabel them.
template = must_replace(template, '<span>Shinobi (PS2 Recom) Launcher - by TheRealAfsan</span>', '<span>{{titleText}}</span>')
template = must_replace(template, '<h2>Shinobi - PS2 Recompilation</h2>', '<h2>{{cardTitle}}</h2>')
template = re.sub(r'<p>Ignite the fiery vengeance.*?</p>', '<p>{{cardText}}</p>', template, count=1, flags=re.S)
template = must_replace(template, '<button class="btn" sc-camel-on-click="{{openCredits}}"><span class="lab">Credits</span></button>',
                        '<div style="display:flex;gap:8px">'
                        '<button class="btn" style="flex:1 1 0;min-width:0" title="The game manual (your own PDF), turned like a book" '
                        'sc-camel-on-click="{{openManual}}" title="In game: F4"><span class="lab">Manual</span></button>'
                        '<button class="btn {{lastCls}}" style="flex:1 1 0;min-width:0" sc-camel-on-click="{{lastBtn}}"><span class="lab">{{lastLabel}}</span></button>'
                        '</div>')
template = must_replace(template, '<button class="btn sm" sc-camel-on-click="{{reset}}"><span class="lab">Reset to Defaults</span></button>',
                        '<div style="display:flex;gap:8px">'
                        '<button class="btn sm {{resetCls}}" style="flex:1 1 0;min-width:0" sc-camel-on-click="{{resetBtn}}"><span class="lab">{{resetLabel}}</span></button>'
                        '<button class="btn sm" style="flex:1 1 0;min-width:0" title="Lighter settings for slower PCs" sc-camel-on-click="{{lowEndBtn}}"><span class="lab">Low-end PC</span></button>'
                        '</div>')

# Mod Menu: Hotsuma's scarf colour (settings [game] scarf_color, ps2_scarf_colour.cpp), above the switches.
template = must_replace(template, '<sc-for list="{{modRows}}" as="m" hint-placeholder-count="4">',
                        '<div class="mrow scarfrow">'
                        '<div><b>Scarf colour</b><span>{{scarfDesc}}</span></div>'
                        '<div style="display:flex;flex-wrap:wrap;gap:6px;align-items:center;justify-content:flex-end;max-width:260px">'
                        '<sc-for list="{{scarfSwatches}}" as="c" hint-placeholder-count="8">'
                        '<button class="swatch" style="{{c.style}}" title="{{c.name}}" aria-label="{{c.name}}" '
                        'aria-pressed="{{c.on}}" sc-camel-on-click="{{c.pick}}"></button>'
                        '</sc-for>'
                        '<input type="color" style="width:30px;height:24px;padding:0;border:1px solid #888;border-radius:4px;background:none;cursor:pointer" value="{{scarfHex}}" title="Any colour" aria-label="Pick any scarf colour" '
                        'sc-camel-on-change="{{scarfPick}}">'
                        '</div></div>'
                        '<sc-for list="{{modRows}}" as="m" hint-placeholder-count="4">')


# ---- LOCAL PATCH: settings panel in three tabs (Display / Graphics / Controller), window mode dropdown, haptics ----
CHK = ('<span class="bx"><svg width="12" height="10" sc-camel-view-box="0 0 12 10" aria-hidden="true"><path d="M1 5l3.5 3.5L11 1.5" '
       'fill="none" stroke="#fff" stroke-width="2.2"></path></svg></span>')
template = must_replace(template, '<h2>Graphical Settings</h2>',
    '<h2>Settings</h2>\n'
    '        <div class="segs tabs" role="tablist" aria-label="Settings pages"><sc-for list="{{tabOpts}}" as="t" hint-placeholder-count="3">'
    '<button class="seg {{t.cls}}" role="tab" aria-selected="{{t.on}}" sc-camel-on-click="{{t.pick}}">{{t.name}}</button></sc-for></div>\n'
    '        <sc-if value="{{tabDisplay}}" hint-placeholder-val="{{true}}">')
_m = re.search(r'\n(\s*)<div( class="inl")?>\s*<div class="fl">Renderer</div>', template)
assert _m, 'renderer anchor'
template = template[:_m.start()] + (
    '\n        <div>\n'
    '          <div class="fl">Window Mode</div>\n'
    '          <div class="selwrap">\n'
    '            <button class="sel" aria-haspopup="listbox" aria-expanded="{{winOpen}}" sc-camel-on-click="{{toggleWin}}">\n'
    '              <span>{{winLabel}}</span>\n'
    '              <svg width="12" height="8" sc-camel-view-box="0 0 12 8" aria-hidden="true"><path d="M1 1l5 5 5-5" fill="none" stroke="#17171a" stroke-width="2"></path></svg>\n'
    '            </button>\n'
    '            <sc-if value="{{winOpen}}" hint-placeholder-val="{{false}}">\n'
    '              <button class="away" tabindex="-1" aria-hidden="true" sc-camel-on-click="{{closeWin}}"></button>\n'
    '              <div class="menu" role="listbox" aria-label="Window mode">\n'
    '                <sc-for list="{{winOptions}}" as="o" hint-placeholder-count="3">\n'
    '                  <button class="opt {{o.cls}}" role="option" aria-selected="{{o.on}}" sc-camel-on-click="{{o.pick}}"><span>{{o.name}}</span><em>{{o.sub}}</em></button>\n'
    '                </sc-for>\n'
    '              </div>\n'
    '            </sc-if>\n'
    '          </div>\n'
    '        </div>\n') + template[_m.start():]
_m = re.search(r'\n\s*<div>\s*<div class="fl"[^>]*><span>Texture Quality', template)
assert _m, 'texture anchor'
template = template[:_m.start()] + '\n        </sc-if>\n        <sc-if value="{{tabGraphics}}" hint-placeholder-val="{{false}}">' + template[_m.start():]
_m = re.search(r'\n\s*<div class="act">', template)
assert _m, 'act anchor'
template = template[:_m.start()] + (
    '\n        </sc-if>\n'
    '        <sc-if value="{{tabController}}" hint-placeholder-val="{{false}}">\n'
    '        <div>\n'
    '          <label class="chk"><input type="checkbox" checked="{{haptics}}" sc-camel-on-change="{{toggleHaptics}}">' + CHK +
    'Haptic Feedback (DualSense)</label>\n'
    '          <p class="note" style="margin:2px 0 6px 30px;font-size:11px;color:#777">Footsteps, shuriken, sword hits, dash and ninja magic, on the left and right of the controller. USB or Bluetooth.</p>\n'
    '        </div>\n'
    '        <div>\n'
    '          <div class="fl"><span>Haptic Strength</span><em>{{hapStrName}}</em></div>\n'
    '          <div class="rng"><div class="trk" style="background: {{hapStrBg}}"></div><input type="range" min="1" max="10" step="1" value="{{hapStr}}" sc-camel-on-change="{{setHapStr}}" aria-label="Haptic strength"></div>\n'
    '        </div>\n'
    '        <div>\n'
    '          <label class="chk"><input type="checkbox" checked="{{trig}}" sc-camel-on-change="{{toggleTrig}}">' + CHK +
    'L2 Trigger Resistance</label>\n'
    '          <p class="note" style="margin:2px 0 0 30px;font-size:11px;color:#777">L2 stiffens part way down and clicks through just before the ninja magic fires.</p>\n'
    '        </div>\n'
    '        </sc-if>') + template[_m.start():]
css_tabs = ('html.manual-only body>*:not([role=dialog]){visibility:hidden!important}html body .pad-focus.pad-focus.pad-focus{outline:3px solid #ff1f3d!important;outline-offset:-3px!important;box-shadow:inset 0 0 14px rgba(255,31,61,.55),0 0 14px rgba(255,31,61,.7)!important;position:relative;z-index:2}.tabs{margin:0 0 6px}.tabs .seg{flex:1 1 auto;min-width:0;padding-left:6px!important;padding-right:6px!important;font-size:12.5px!important;letter-spacing:0!important}html.tabbed .grp>*{animation:none!important;opacity:1!important}'
            '.grp>sc-if>div,.grp sc-if>div{margin-bottom:2px}')
template = must_replace(template, '@media (prefers-reduced-motion:reduce)', css_tabs + '@media (prefers-reduced-motion:reduce)')


# ---- LOCAL PATCH: hero slideshow. The key art, then the pictures in Pictures\lauccher art N.jpg (N = 2, 3, ...),
# crossfading every 5 seconds, in a loop. Each picture is a layer over the key art, under the shading.
_slides = []
_pics = os.path.join(os.environ['USERPROFILE'], 'Pictures')
for _n in range(2, 20):
    _f = os.path.join(_pics, 'lauccher art %d.jpg' % _n)
    if not os.path.exists(_f):
        continue
    _im = Image.open(_f).convert('RGB')
    if _im.height > 1200:
        _im = _im.resize((round(_im.width * 1200 / _im.height), 1200), Image.LANCZOS)
    _uid = str(uuid.uuid4())
    manifest[_uid] = {'mime': 'image/jpeg', 'compressed': False, 'data': base64.b64encode(_jpg(_im)).decode()}
    _slides.append(_uid)
if _slides:
    _hold = 5.0
    _cycle = _hold * (len(_slides) + 1)
    _imgs = ''.join('\n      <img class="slide" src="%s" alt="" aria-hidden="true" style="animation-delay:%.1fs">' % (u, _hold * (i + 1))
                    for i, u in enumerate(_slides))
    _m = re.search(r'(<img class="art" src="[^"]+" alt="[^"]*">)', template)
    assert _m, 'hero art'
    template = template[:_m.end()] + _imgs + template[_m.end():]
    _fade = 1.0 / _cycle * 100.0          # one second, in percent of the cycle
    _show = _hold / _cycle * 100.0
    css_slides = ('.hero .slide{position:absolute;inset:0;width:100%%;height:100%%;object-fit:cover;opacity:0;'
                  'animation:heroSlide %.1fs linear infinite;pointer-events:none}'
                  '@keyframes heroSlide{0%%{opacity:0}%.2f%%{opacity:1}%.2f%%{opacity:1}%.2f%%{opacity:0}100%%{opacity:0}}'
                  % (_cycle, _fade, _show, _show + _fade))
    template = must_replace(template, '@media (prefers-reduced-motion:reduce)', css_slides + '@media (prefers-reduced-motion:reduce)')
    print('hero slideshow:', len(_slides), 'pictures')


# ---- LOCAL PATCH: Graphics tab gets Blending + Texture Filtering; a new Effects tab (screen filter, dithering,
# mipmapping, upscaling fixes) ----
def _segrow(label, binding, count, tip=''):
    return ('        <div>\n'
            '          <div class="fl"' + (' title="%s"' % tip if tip else '') + '>' + label + '</div>\n'
            '          <div class="segs" role="group" aria-label="' + label + '">\n'
            '            <sc-for list="{{' + binding + '}}" as="o" hint-placeholder-count="' + str(count) + '">\n'
            '              <button class="seg {{o.cls}}" aria-pressed="{{o.on}}" sc-camel-on-click="{{o.pick}}">{{o.name}}</button>\n'
            '            </sc-for>\n'
            '          </div>\n'
            '        </div>\n')
def _chkrow(label, binding, toggle, note):
    return ('        <div>\n'
            '          <label class="chk"><input type="checkbox" checked="{{' + binding + '}}" sc-camel-on-change="{{' + toggle + '}}">' + CHK + label + '</label>\n'
            '          <p class="note" style="margin:0 0 4px 30px;font-size:11px;color:#777">' + note + '</p>\n'
            '        </div>\n')
template = must_replace(template, '        </sc-if>\n        <sc-if value="{{tabController}}"',
    _segrow('Blending Accuracy', 'blendOpts', 4, 'How exactly glows, smoke and see-through effects are drawn. Higher costs more GPU.') +
    _segrow('Texture Filtering', 'texfOpts', 2, 'As on PS2 keeps each texture as the game asked; Bilinear smooths them all.') +
    '        </sc-if>\n'
    '        <sc-if value="{{tabEffects}}" hint-placeholder-val="{{false}}">\n' +
    _segrow('Screen Filter', 'scrfOpts', 4, 'Sharp shows the pixels; Smooth blends them; Scanlines and CRT look like an old TV.') +
    _chkrow('Dithering', 'dith', 'toggleDith', 'The PS2&#39;s grainy colour pattern. Untick for smooth, clean gradients.') +
    _chkrow('Mipmapping', 'mip', 'toggleMip', 'Less shimmer on distant floors and walls. Applies after a restart.') +
    _chkrow('Upscaling Fixes', 'upfix', 'toggleUpfix', 'Removes seams and thin lines at high resolutions. Applies after a restart.') +
    '        </sc-if>\n        <sc-if value="{{tabController}}"')

# ---- LOCAL PATCH: performance overlay switch (Display tab) ----
_i = template.index('V-Sync</label>') + len('V-Sync</label>')
template = template[:_i] + ('\n          <label class="chk" title="Also F2 in game"><input type="checkbox" checked="{{ovl}}" sc-camel-on-change="{{toggleOvl}}">'
                            + CHK + 'Performance Overlay (F2)</label>'
                            + '\n          <label class="chk" title="Also F3 in game"><input type="checkbox" checked="{{cam}}" sc-camel-on-change="{{toggleCam}}">'
                            + CHK + 'Close-Up Camera (F3)</label>') + template[_i:]

# Bottom of the left column: the ownership notice above the PlayStation / SEGA logos, the studio and author lines below.
template = must_replace(template,
                        '      <div class="foot">\n'
                        '        <div style="display:flex;align-items:center;gap:14px">',
                        '      <div class="foot-wrap">\n'
                        '      <p class="own">Requires your own original copy of Shinobi (PS2).<br>Fan project, not affiliated '
                        'with SEGA or Sony.</p>\n'
                        '      <div class="foot">\n'
                        '        <div style="display:flex;align-items:center;gap:14px">')
template = must_replace(template,
                        '<span class="tra-b">TR<i>A</i></span></div>\n      </div>\n    </div>',
                        '<span class="tra-b">TR<i>A</i></span></div>\n      </div>\n'
                        '      <p class="studio">Developed by <b>Twilight Orchard</b></p>\n'
                        '      <p class="studio">Created by <b>TheRealAfsan</b></p>\n'
                        '      </div>\n    </div>')
template = must_replace(template, '<p>Launcher and recompilation tooling by TheRealAfsan.</p>',
                        '<p>Developed by Twilight Orchard. Created by TheRealAfsan.</p>\n'
                        '                <p>Launcher and recompilation tooling by TheRealAfsan.</p>')

# "Effects Quality" is the picture sharpening (the game's own shader): named for what it does.
template = must_replace(template, '<div class="fl"><span>Effects Quality</span><em>{{fxName}}</em></div>',
                        '<div class="fl"><span>Sharpening</span><em>{{fxName}}</em></div>')
template = must_replace(template, 'aria-label="Effects quality"', 'aria-label="Sharpening"')
template = must_replace(template, '<div class="fl"><span>Texture Quality</span><em>{{texName}}</em></div>',
                        '<div class="fl" title="Anisotropic filtering: sharper floors and walls seen at an angle">'
                        '<span>Texture Quality</span><em>{{texName}}</em></div>')

# Renderer and the new Frame Rate as one-line rows (label left, buttons right) so the panel still fits.
template = must_replace(template, '<div>\n          <div class="fl">Renderer</div>', '<div class="inl">\n          <div class="fl">Renderer</div>')
template = must_replace(template, '        <div>\n          <label class="chk"><input type="checkbox" checked="{{ws}}"',
                        '        <div class="inl">\n          <div class="fl">Frame Rate</div>\n'
                        '          <div class="segs" role="group" aria-label="Frame rate">\n'
                        '            <sc-for list="{{fpsOpts}}" as="o" hint-placeholder-count="4">\n'
                        '              <button class="seg {{o.cls}}" aria-pressed="{{o.on}}" title="{{o.tip}}" '
                        'sc-camel-on-click="{{o.pick}}">{{o.name}}</button>\n'
                        '            </sc-for>\n          </div>\n        </div>\n\n'
                        '        <div>\n          <label class="chk"><input type="checkbox" checked="{{ws}}"')
# Widescreen: the label names the shape; chips for 16:9 / 21:9 / 32:9 / Auto in the design's MSAA-chip style.
template = must_replace(template, 'Widescreen Hack (16:9)</label>',
                        'Widescreen Hack ({{wsName}})</label>\n'
                        '          <sc-if value="{{ws}}" hint-placeholder-val="{{false}}"><div class="sub" role="group" '
                        'aria-label="Widescreen shape" style="margin:0 0 4px"><sc-for list="{{wsOpts}}" as="w" '
                        'hint-placeholder-count="4"><button class="chipb {{w.cls}}" aria-pressed="{{w.on}}" '
                        'sc-camel-on-click="{{w.pick}}">{{w.name}}</button></sc-for></div></sc-if>')
template = must_replace(template, '<p>Launcher and recompilation tooling by TheRealAfsan.</p>',
                        '<p>Launcher and recompilation tooling by TheRealAfsan.</p>\n'
                        '                <h4>Built with</h4>\n'
                        '                <p>PS2Recomp (ps2xRecomp / ps2xRuntime). Graphics: PCSX2\'s GS through the '
                        'pcsx2-gs-bridge; sound: PCSX2\'s SPU2 core (both GPL-3.0+, PCSX2 Dev Team). raylib, Microsoft '
                        'WebView2.</p>')

# ---------------------------------------------------------------- game file (the user's own disc image)
# A "Game File" button and dialog: Browse... for the Shinobi (USA) disc image, check that it is the right game, Set up copies
# the game's files out of it (launcher_iso.cpp). Nothing of the game ships with this program.
template = must_replace(template, '@media (prefers-reduced-motion:reduce)',
                        '.pathrow{display:flex;align-items:center;gap:10px;margin:12px 0 4px}.pathrow .btn{width:auto;flex:0 0 auto;padding:0 26px}'
                        '.pathtxt{flex:1;min-width:0;padding:9px 11px;border:1px solid #b5b5bb;background:#f2f2f4;color:#222;'
                        'font-size:12.5px;overflow:hidden;text-overflow:ellipsis;white-space:nowrap;direction:rtl;text-align:left}'
                        '.gamebox .go{margin-top:14px;min-width:240px;position:relative;overflow:hidden}'
                        '@media (prefers-reduced-motion:reduce)')
template = must_replace(template,
                        '<button class="btn" sc-camel-on-click="{{openMods}}"><span class="lab">Mod Menu</span></button>',
                        '<button class="btn" sc-camel-on-click="{{openMods}}"><span class="lab">Mod Menu</span></button>\n'
                        '        <button class="btn {{gameBtnCls}}" sc-camel-on-click="{{openGame}}"><span class="lab">{{gameBtnLabel}}</span></button>')
template = must_replace(template, '            <sc-if value="{{showCredits}}" hint-placeholder-val="{{false}}">',
                        '            <sc-if value="{{showGame}}" hint-placeholder-val="{{false}}">\n'
                        '              <div class="gamebox">\n'
                        '                <p class="note" style="margin-top:0">{{gameIntro}}</p>\n'
                        '                <div style="display:flex;flex-direction:column;gap:5px;margin:10px 0 2px">'
                        '<sc-for list="{{gameSteps}}" as="g" hint-placeholder-count="3">'
                        '<div style="display:flex;align-items:center;gap:10px;font-size:13.5px">'
                        '<span style="{{g.mark}}">{{g.icon}}</span><b style="{{g.text}}">{{g.label}}</b></div>'
                        '</sc-for></div>\n'
                        '                <div class="pathrow"><span class="pathtxt" title="{{isoPath}}">{{isoPath}}</span>'
                        '<button class="btn sm" sc-camel-on-click="{{browseIso}}"><span class="lab">Browse...</span></button></div>\n'
                        '                <p class="note">{{isoStatus}}</p>\n'
                        '                <label style="display:flex;align-items:center;gap:10px;margin:12px 0 0;cursor:pointer;font-size:13.5px">'
                        '<input type="checkbox" checked="{{owned}}" sc-camel-on-change="{{toggleOwned}}" '
                        'style="width:18px;height:18px;accent-color:#b3122a;cursor:pointer" aria-label="I own this game">'
                        '<span>I own this copy of Shinobi and made the disc image from my own disc.</span></label>\n'
                        '                <button class="btn go" disabled="{{installBusy}}" sc-camel-on-click="{{installIso}}">'
                        '<sc-if value="{{installBusy}}" hint-placeholder-val="{{false}}"><span class="fill" style="width: {{installFill}}"></span></sc-if>'
                        '<span class="lab">{{installLabel}}</span></button>\n'
                        '                <p class="note">{{installNote}}</p>\n'
                        '                <div class="pathrow"><span class="pathtxt" title="{{savesPath}}">{{savesPath}}</span>'
                        '<button class="btn sm" sc-camel-on-click="{{openSaves}}"><span class="lab">Open saves folder</span></button></div>\n'
                        '                <p class="note">Your saves are kept in this folder, apart from the game files, so they stay yours when the game files are set up again.</p>\n'
                        '              </div>\n'
                        '            </sc-if>\n\n'
                        '            <sc-if value="{{showCredits}}" hint-placeholder-val="{{false}}">')

# ---------------------------------------------------------------- logic
# Left column: five buttons no longer fit, so tighter buttons and gaps; the saved game file's place sits under its button.
template = must_replace(template, '@media (prefers-reduced-motion:reduce)',
                        '.left .btns{gap:6px}.left{gap:8px}.th{height:50px}.card{padding:9px 12px 10px}.card p{font-size:12px;line-height:1.3}'
                        '.foot img[alt="PlayStation"]{height:26px}'
                        '.gfile{margin:-2px 0 0;font-family:Rajdhani,sans-serif;font-size:11px;font-weight:600;color:#4a4a52;'
                        'overflow:hidden;text-overflow:ellipsis;white-space:nowrap;padding-left:2px}'
                        '.gfile.warn{color:#b3122a}'
                        '@media (prefers-reduced-motion:reduce)')
template = must_replace(template,
                        '<button class="btn {{gameBtnCls}}" sc-camel-on-click="{{openGame}}"><span class="lab">{{gameBtnLabel}}</span></button>',
                        '<button class="btn {{gameBtnCls}}" sc-camel-on-click="{{openGame}}"><span class="lab">{{gameBtnLabel}}</span></button>\n'
                        '        <p class="gfile {{gfileCls}}" title="{{isoPath}}">{{gameCaption}}</p>')

logic_start = template.index('class Component extends DCLogic')
logic_end = template.index('</script>', logic_start)
logic = template[logic_start:logic_end]


def L(old, new, count=1):
    global logic
    logic = must_replace(logic, old, new, count)


# Host bridge, initial state from the host, real defaults.
L("""    this.defaults = { res: '1080p', renderer: 'Vulkan', ws: true, vsync: false, tex: 2, fx: 3, aa: 'Off', msaa: 4 };
    this.defBinds = { up: 'W', down: 'S', left: 'A', right: 'D', cross: 'K', circle: 'L', square: 'J', triangle: 'I', l1: 'Q', r1: 'E', l2: 'Z', r2: 'C', start: 'Enter', select: 'Backspace' };
    this.state = Object.assign({}, this.defaults, {
      saved: this.sig(this.defaults),
      resOpen: false, modal: null, guide: 0,
      launch: 'idle', progress: 0, toast: '',
      device: 'keyboard', listening: null,
      bind: Object.assign({}, this.defBinds),
      mods: { intro: true, fps: false, tex: false, dbg: false }
    });""",
  """    // Defaults: the game's own (ps2_settings.cpp) where it has them.
    this.defaults = { res: '1080p', renderer: 'D3D11', fps: '60', ws: true, wsShape: '16:9', vsync: true, tex: 1, fx: 1, aa: 'Off', msaa: 4, blend: 'basic' };
    this.defBinds = { up: 'UP,W', down: 'DOWN,S', left: 'LEFT,A', right: 'RIGHT,D', cross: 'X,SPACE', circle: 'C,ESCAPE', square: 'Z,KP_0', triangle: 'V,KP_1', l1: 'Q', r1: 'E', l2: 'LEFT_SHIFT', r2: 'RIGHT_SHIFT', start: 'ENTER', select: 'TAB' };
    this.defMods = { studio: true, nscale: true, rr5fix: false, dropst: false, record: false, intro: false, fastvu: false, dbg: false, full: true, diag: false, legacy: false, k8: false };
    const init = window.__launcherInit || null;
    this.manualOnly = !!(init && init.manualOnly);
    const start = Object.assign({}, this.defaults, init ? init.settings : {});
    this.state = Object.assign({}, start, {
      resOpen: false, modal: null, guide: 0,
      launch: 'idle', progress: 0, toast: '',
      device: 'keyboard', listening: null,
      bind: Object.assign({}, this.defBinds, init ? init.binds : {}),
      mods: Object.assign({}, this.defMods, init ? init.mods : {})
    });
    this.state.saved = this.sig(this.state);
    this.host = (window.chrome && window.chrome.webview) ? window.chrome.webview : null;
    if (this.host) {
      this.host.addEventListener('message', (e) => this.onHost(e.data));
    }
    // In-game mode: the game's pause menu (ps2_ingame_menu.cpp opens this page with init.ingame).
    this.ingame = !!(init && init.ingame);
    this.state.restart = !!(init && init.restart);
    this.state.quitArmed = false;
    document.documentElement.classList.toggle('ingame', this.ingame);
    this.navSetup();
    // Launcher music (launcher_assets/music.*, found by the host): loops while the launcher is open, fades in, pauses
    // while the game runs. The mute button in the title bar is remembered (launcher.ini).
    this.state.muted = !!(init && init.muted);
    document.documentElement.classList.toggle('muted', this.state.muted);
    this.music = null;
    // The host names the file (null = none); a host that does not know about music yet -> music.mp3 beside this page.
    const musicSrc = (init && 'music' in init) ? init.music : 'music.mp3';
    if (musicSrc) {
      this.music = new Audio(musicSrc);
      this.music.addEventListener('error', () => { this.music = null; document.documentElement.classList.add('nomusic'); });
      this.music.loop = true;
      this.music.volume = 0;
      this.music.muted = this.state.muted;
      this.musicOn();
    } else {
      document.documentElement.classList.add('nomusic');
    }""")

L("""  sig(s) {""",
  """  musicFade(to, ms, then) {
    if (!this.music) return;
    clearInterval(this.fadeTimer);
    const from = this.music.volume, start = Date.now();
    this.fadeTimer = setInterval(() => {
      const k = Math.min((Date.now() - start) / ms, 1);
      this.music.volume = from + (to - from) * k;
      if (k >= 1) { clearInterval(this.fadeTimer); if (then) then(); }
    }, 30);
  }

  musicOn() {
    if (!this.music) return;
    const p = this.music.play();
    // If the browser still wants a click before sound, start on the first one.
    if (p && p.catch) p.catch(() => document.addEventListener('pointerdown', () => this.musicOn(), { once: true }));
    this.musicFade(0.12, 1500); // 12% volume (the track is mastered loud)
  }

  musicOff() {
    if (!this.music) return;
    this.musicFade(0, 600, () => this.music.pause());
  }

  // Controller (Gamepad API) and arrow-key navigation, for the in-game menu and the launcher alike. Up/down move through
  // the controls in page order (inside the open dialog if there is one); left/right step sliders and pick the next/previous
  // option of a segmented row, else move too; Cross / Enter presses; Circle / Escape closes the dialog (in game: resumes);
  // Start resumes in game. A red outline shows where you are.
  navItems() {
    const scope = document.querySelector('.menu') || document.querySelector('.dlg') || document;
    return Array.from(scope.querySelectorAll('button, input, [tabindex="0"]')).filter((el) => {
      if (el.disabled || el.classList.contains('scrim-bg') || el.classList.contains('away')) return false;
      if (el.closest('.tb')) return false;
      const r = el.getBoundingClientRect();
      return r.width > 0 && r.height > 0 && getComputedStyle(el).visibility !== 'hidden';
    });
  }

  navFocus(el) {
    if (!el) return;
    document.querySelectorAll('.pad-focus').forEach((e) => e.classList.remove('pad-focus'));
    const target = el.matches('input[type=checkbox]') ? (el.closest('label') || el) : el;
    target.classList.add('pad-focus');
    this.navCurrent = el;
    // Remembered by position and text, to put the marker back when the page redraws its controls.
    const all = this.navItems();
    this.navIdx = all.indexOf(el);
    this.navTxt = (target.textContent || '').trim();
    el.focus({ preventScroll: true });
    target.scrollIntoView({ block: 'nearest' });
  }

  // After a redraw the marked control is a new element: find it again (same text, nearest place) and mark it.
  navRestore() {
    if (this.navIdx === undefined || this.navIdx < 0) return;
    const cur = this.navCurrent;
    const curTarget = cur && (cur.matches('input[type=checkbox]') ? (cur.closest('label') || cur) : cur);
    if (cur && document.contains(cur) && curTarget.classList.contains('pad-focus')) return;
    const items = this.navItems();
    let best = null, bestD = Infinity;
    items.forEach((el, i) => {
      const t = el.matches('input[type=checkbox]') ? (el.closest('label') || el) : el;
      if ((t.textContent || '').trim() !== this.navTxt) return;
      const d = Math.abs(i - this.navIdx);
      if (d < bestD) { bestD = d; best = el; }
    });
    if (!best) best = items[Math.min(this.navIdx, items.length - 1)];
    if (best) {
      document.querySelectorAll('.pad-focus').forEach((e) => e.classList.remove('pad-focus'));
      (best.matches('input[type=checkbox]') ? (best.closest('label') || best) : best).classList.add('pad-focus');
      this.navCurrent = best;
    }
  }

  navMove(delta) {
    const items = this.navItems();
    if (!items.length) return;
    let i = items.indexOf(this.navCurrent);
    i = i < 0 ? 0 : (i + delta + items.length) % items.length;
    this.navFocus(items[i]);
  }

  // Moves to the nearest control in direction (dx, dy): candidates must lie that way; ones overlapping the current
  // control across the move (same row for left/right, same column for up/down) win, then the closest.
  navDir(dx, dy) {
    const items = this.navItems();
    if (!items.length) return;
    const cur = this.navCurrent && document.contains(this.navCurrent) ? this.navCurrent : null;
    if (!cur) { this.navFocus(items[0]); return; }
    const box = (el) => (el.matches('input[type=checkbox]') ? (el.closest('label') || el) : el).getBoundingClientRect();
    const a = box(cur);
    const ac = { x: (a.left + a.right) / 2, y: (a.top + a.bottom) / 2 };
    let best = null, bestScore = Infinity;
    for (const el of items) {
      if (el === cur) continue;
      const r = box(el);
      if (r.width === 0 && r.height === 0) continue;
      const c = { x: (r.left + r.right) / 2, y: (r.top + r.bottom) / 2 };
      // How far it is in the pressed direction (edge to edge), and how far off to the side.
      const along = dx ? (dx > 0 ? r.left - a.right : a.left - r.right) : (dy > 0 ? r.top - a.bottom : a.top - r.bottom);
      const ahead = dx ? (dx > 0 ? c.x - ac.x : ac.x - c.x) : (dy > 0 ? c.y - ac.y : ac.y - c.y);
      if (ahead <= 4 || along < -Math.min(a.width, a.height, r.width, r.height) / 2) continue;
      const overlap = dx ? Math.min(a.bottom, r.bottom) - Math.max(a.top, r.top) : Math.min(a.right, r.right) - Math.max(a.left, r.left);
      const side = overlap > 0 ? 0 : (dx ? Math.abs(c.y - ac.y) : Math.abs(c.x - ac.x));
      const score = Math.max(0, along) + side * 3 + (overlap > 0 ? 0 : 400);
      if (score < bestScore) { bestScore = score; best = el; }
    }
    if (best) this.navFocus(best);
  }

  navSide(delta) {
    const el = this.navCurrent;
    if (el && el.matches('input[type=range]')) {
      const v = Math.min(Number(el.max), Math.max(Number(el.min), Number(el.value) + delta));
      Object.getOwnPropertyDescriptor(HTMLInputElement.prototype, 'value').set.call(el, String(v));
      el.dispatchEvent(new Event('input', { bubbles: true }));
      el.dispatchEvent(new Event('change', { bubbles: true }));
      return;
    }
    const group = el && el.parentElement && (el.matches('.seg, .chipb') ? el.parentElement : null);
    if (group) {
      const sibs = Array.from(group.querySelectorAll('.seg, .chipb'));
      // Left / right move between the options of a row; Cross picks one (as on a console).
      const next = sibs[sibs.indexOf(el) + delta];
      if (next) { this.navFocus(next); return; }
    }
    this.navDir(delta, 0);
  }

  navBack() {
    const x = document.querySelector('.dlg .x');
    if (x) { x.click(); setTimeout(() => this.navMove(0), 0); return; }
    if (this.state.resOpen || this.state.winOpen) {
      this.setState({ resOpen: false, winOpen: false });
      setTimeout(() => { const o = this.navOpener && document.contains(this.navOpener) ? this.navOpener : document.querySelector('.sel'); if (o) this.navFocus(o); }, 30);
      return;
    }
    if (this.ingame && this.host) this.host.postMessage('resume');
  }

  navPress() {
    const el = this.navCurrent;
    if (!el || !document.contains(el)) { this.navMove(0); return; }
    const opener = el.matches('.sel') ? el : null;
    el.click();
    if (opener) {
      this.navOpener = opener;
      setTimeout(() => { const on = document.querySelector('.menu .opt.on') || document.querySelector('.menu .opt'); if (on) this.navFocus(on); }, 30);
      return;
    }
    // A pick in an open list closes it: back to its button.
    if (el.matches('.opt') && this.navOpener) {
      setTimeout(() => { if (document.contains(this.navOpener)) this.navFocus(this.navOpener); }, 30);
      return;
    }
    setTimeout(() => { if (!document.contains(this.navCurrent)) this.navMove(0); }, 0);
  }

  navSetup() {
    document.addEventListener('keydown', (e) => {
      if (this.state && this.state.listening) return; // binding a key in Configure Controller
      const k = e.key;
      if (k === 'ArrowDown' || k === 'ArrowUp') { e.preventDefault(); this.navDir(0, k === 'ArrowDown' ? 1 : -1); }
      else if (k === 'ArrowLeft' || k === 'ArrowRight') {
        if (this.navCurrent && this.navCurrent.matches('input[type=range]')) return; // the slider's own keys
        e.preventDefault(); this.navSide(k === 'ArrowRight' ? 1 : -1);
      }
      else if (k === 'Escape') { e.preventDefault(); this.navBack(); }
      else if (k === 'F1' && this.ingame) { e.preventDefault(); if (this.host) this.host.postMessage('resume'); }
    });
    const held = {};
    const REPEAT_FIRST = 380, REPEAT_NEXT = 110;
    const edge = (name, down, fn) => {
      const now = performance.now();
      const h = held[name];
      if (!down) { held[name] = null; return; }
      if (!h) { held[name] = { next: now + REPEAT_FIRST }; fn(); return; }
      if (now >= h.next) { h.next = now + REPEAT_NEXT; fn(); }
    };
    let primed = false;
    window.__nav = this;
    this.navShow = () => { if (!this.navCurrent || !document.contains(this.navCurrent)) { this.navMove(0); return true; } return false; };
    const poll = () => {
      const pads = (navigator.getGamepads ? Array.from(navigator.getGamepads()).filter(Boolean) : [])
        .filter((p) => !(this.hostPad && /054c/i.test(p.id || ''))); // a DualSense the launcher reads itself
      if (pads.length && !primed) {
        // Buttons already down when the page starts (Select+Start that opened the menu) count only after a release.
        primed = true;
        [[0, 'cross'], [1, 'circle'], [9, 'start'], [12, 'up'], [13, 'down'], [14, 'left'], [15, 'right']].forEach(([i, n]) => {
          if (pads.some((p0) => p0.buttons[i] && p0.buttons[i].pressed)) held[n] = { next: Infinity };
        });
      }
      // While the game is starting or running the controller belongs to the game: Cross would press the launcher's Stop Game button.
      const gameBusy = !this.ingame && this.state && this.state.launch !== 'idle';
      if (pads.length && !gameBusy && !this.ingame && !(this.state && this.state.listening)) {
        const RAW = { 0: 1, 1: 2, 2: 0, 3: 3, 9: 9 }; // standard index -> raw Sony index
        const hat = (p) => { const v = p.axes[9]; if (v === undefined || v > 1.05 || v < -1.05) return -1; return Math.round((v + 1) * 3.5) % 8; };
        const press = (p, i) => {
          if (p.mapping === 'standard') return !!(p.buttons[i] && p.buttons[i].pressed);
          if (i >= 12 && i <= 15) {
            const h = hat(p);
            if (h < 0) return false;
            return i === 12 ? (h === 7 || h === 0 || h === 1) : i === 15 ? (h >= 1 && h <= 3) : i === 13 ? (h >= 3 && h <= 5) : (h >= 5 && h <= 7);
          }
          const r = RAW[i] !== undefined ? RAW[i] : i;
          return !!(p.buttons[r] && p.buttons[r].pressed);
        };
        const btn = (i) => pads.some((p) => press(p, i));
        const ax = (i) => pads.reduce((best, p) => { const v = p.axes[i] || 0; return Math.abs(v) > Math.abs(best) ? v : best; }, 0);
        const manual = document.documentElement.classList.contains('manual-open') && this._book;
        if (manual) {
          const turn = (key) => document.dispatchEvent(new KeyboardEvent('keydown', { key: key, bubbles: true, cancelable: true }));
          edge('left', btn(14) || ax(0) < -0.6, () => turn('ArrowLeft'));
          edge('right', btn(15) || ax(0) > 0.6, () => turn('ArrowRight'));
          edge('circle', btn(1), () => this.manualClose());
        } else {
        edge('up', btn(12) || ax(1) < -0.6, () => { if (!this.navShow()) this.navDir(0, -1); });
        edge('down', btn(13) || ax(1) > 0.6, () => { if (!this.navShow()) this.navDir(0, 1); });
        edge('left', btn(14) || ax(0) < -0.6, () => this.navSide(-1));
        edge('right', btn(15) || ax(0) > 0.6, () => this.navSide(1));
        edge('cross', btn(0), () => this.navPress());
        const tabStep = (d) => {
          const tabs = Array.from(document.querySelectorAll('.tabs .seg'));
          if (!tabs.length || document.querySelector('.dlg')) return;
          const i = Math.max(0, tabs.findIndex((t) => t.classList.contains('on')));
          const t = tabs[(i + d + tabs.length) % tabs.length];
          t.click();
          setTimeout(() => this.navFocus(document.querySelectorAll('.tabs .seg')[(i + d + tabs.length) % tabs.length]), 30);
        };
        edge('l1', btn(4), () => tabStep(-1));
        edge('r1', btn(5), () => tabStep(1));
        edge('circle', btn(1), () => this.navBack());
        }
        edge('start', btn(9), () => { if (this.ingame && this.host) this.host.postMessage('resume'); });
      }
      this.navRestore();
      requestAnimationFrame(poll);
    };
    requestAnimationFrame(poll);
    // In game the menu starts on Resume.
    if (this.ingame && !this.manualOnly) setTimeout(() => this.navFocus(document.querySelector('.btn.go')), 150);
    if (this.manualOnly) {
      document.documentElement.classList.add('manual-only');
      setTimeout(() => { if (this.host) this.host.postMessage('manual'); }, 30);
    }
  }

  sig(s) {""")

L("""  sig(s) {
    return JSON.stringify({ res: s.res, renderer: s.renderer, ws: s.ws, vsync: s.vsync, tex: s.tex, fx: s.fx, aa: s.aa, msaa: s.msaa });
  }""",
  """  sig(s) {
    return JSON.stringify({ res: s.res, renderer: s.renderer, fps: s.fps, ws: s.ws, wsShape: s.wsShape, vsync: s.vsync, tex: s.tex, fx: s.fx, aa: s.aa, blend: s.blend, scarf: s.scarf || '', bind: s.bind, mods: s.mods });
  }

  send(cmd, extra) {
    if (!this.host) { this.toastMsg('Launcher host not connected'); return; }
    const s = this.state;
    const lines = [cmd];
    const add = (k, v) => lines.push(k + '=' + String(v));
    add('res', s.res); add('renderer', s.renderer); add('fps', s.fps); add('ws', s.ws ? 1 : 0); add('wsShape', s.wsShape);
    add('vsync', s.vsync ? 1 : 0); add('tex', s.tex); add('fx', s.fx); add('aa', s.aa); add('blend', s.blend || 'basic');
    add('scarf', s.scarf || 'original');
    Object.keys(s.mods).forEach((k) => add('mod.' + k, s.mods[k] ? 1 : 0));
    Object.keys(s.bind).forEach((k) => add('bind.' + k, s.bind[k]));
    if (extra) Object.keys(extra).forEach((k) => add(k, extra[k]));
    this.host.postMessage(lines.join('\\n'));
  }

  // The F1 menu's controller, read by the game and sent here (see ps2_ingame_menu.h padEvent).
  padCmd(name) {
    // The launcher reads a DualSense itself (launcher_dualsense.cpp): from now on the page's own Gamepad API leaves Sony pads to it.
    if (name === 'host') { this.hostPad = true; return; }
    if (name === 'lost') { this.hostPad = false; return; }
    // While the game is starting or running the controller belongs to the game.
    if (!this.ingame && this.state && this.state.launch !== 'idle') return;
    const manual = document.documentElement.classList.contains('manual-open') && this._book;
    const key = (k) => document.dispatchEvent(new KeyboardEvent('keydown', { key: k, bubbles: true, cancelable: true }));
    if (manual) {
      if (name === 'left') key('ArrowLeft');
      else if (name === 'right') key('ArrowRight');
      else if (name === 'circle') this.manualClose();
      return;
    }
    if (name === 'up') { if (!this.navShow || !this.navShow()) this.navDir(0, -1); }
    else if (name === 'down') { if (!this.navShow || !this.navShow()) this.navDir(0, 1); }
    else if (name === 'left') this.navSide(-1);
    else if (name === 'right') this.navSide(1);
    else if (name === 'cross') this.navPress();
    else if (name === 'circle') this.navBack();
    else if (name === 'l1' || name === 'r1') this.navTab(name === 'l1' ? -1 : 1);
    else if (name === 'start' && this.ingame && this.host) this.host.postMessage('resume');
  }

  navTab(d) {
    const tabs = Array.from(document.querySelectorAll('.tabs .seg'));
    if (!tabs.length || document.querySelector('.dlg')) return;
    const i = Math.max(0, tabs.findIndex((t) => t.classList.contains('on')));
    const n = (i + d + tabs.length) % tabs.length;
    tabs[n].click();
    setTimeout(() => this.navFocus(document.querySelectorAll('.tabs .seg')[n]), 30);
  }

  onHost(msg) {
    const m = typeof msg === 'string' ? msg : '';
    if (m.indexOf('pad:') === 0) { this.padCmd(m.slice(4)); return; }
    if (m === 'manualhide') { this.manualHide(); return; }
    if (m === 'max:1' || m === 'max:0') { document.documentElement.classList.toggle('maxed', m === 'max:1'); return; }
    if (m === 'saved') { this.setState({ saved: this.sig(this.state) }); this.toastMsg('Settings saved'); }
    else if (m === 'saved:restart') { this.setState({ saved: this.sig(this.state), restart: true }); this.toastMsg('Saved - some changes apply after a restart'); }
    else if (m === 'saved:live') { this.setState({ saved: this.sig(this.state) }); this.toastMsg('Settings applied'); }
    else if (m === 'starting') { this.setState({ launch: 'launching', progress: 10 }); this.musicOff(); }
    else if (m === 'running') { clearInterval(this.timer); this.setState({ launch: 'running', progress: 100 }); this.musicOff(); }
    else if (m === 'exited') { clearInterval(this.timer); this.setState({ launch: 'idle', progress: 0 }); this.toastMsg('Game closed'); this.musicOn(); }
    else if (m.indexOf('manual-progress:') === 0) { const a = m.slice(16).split('|'); this.toastMsg(+a[1] ? 'Preparing the manual... page ' + a[0] + ' of ' + a[1] : 'Preparing the manual...'); }
    else if (m.indexOf('manual-error:') === 0) { this.toastMsg(m.slice(13)); }
    else if (m.indexOf('manual:') === 0) { const a = m.slice(7).split('|'); this.manualShow(+a[0], a[1], a[2] || ''); }
    else if (m.indexOf('error:') === 0) { clearInterval(this.timer); this.setState({ launch: 'idle', progress: 0 }); this.toastMsg(m.slice(6)); this.musicOn(); }
  }""")

# Key names as the game's settings.ini writes them (raylib names).
L("""  keyName(e) {
    const k = e.key;
    const map = { ' ': 'Space', ArrowUp: 'Up', ArrowDown: 'Down', ArrowLeft: 'Left', ArrowRight: 'Right' };
    if (map[k]) return map[k];
    return k.length === 1 ? k.toUpperCase() : k;
  }""",
  """  keyName(e) {
    const k = e.key;
    const map = { ' ': 'SPACE', ArrowUp: 'UP', ArrowDown: 'DOWN', ArrowLeft: 'LEFT', ArrowRight: 'RIGHT', Enter: 'ENTER',
      Backspace: 'BACKSPACE', Tab: 'TAB', Insert: 'INSERT', Delete: 'DELETE', Home: 'HOME', End: 'END', PageUp: 'PAGE_UP', PageDown: 'PAGE_DOWN' };
    if (map[k]) return map[k];
    if (e.code && e.code.indexOf('Numpad') === 0 && /^[0-9]$/.test(k)) return 'KP_' + k;
    return k.length === 1 ? k.toUpperCase() : k.toUpperCase();
  }""")

L("""    const RES = [['720p', '1280 x 720'], ['1080p', '1920 x 1080'], ['1440p', '2560 x 1440'], ['4K', '3840 x 2160']];""",
  """    // The PCSX2 renderer draws at a whole multiple of the PS2's 448 lines.
    const RES = [['Native', '1x  640 x 448'], ['720p', '2x  1280 x 896'], ['1080p', '3x  1920 x 1344'], ['1440p', '4x  2560 x 1792'], ['4K', '6x  3840 x 2688']]
      // 8K (12x): PCSX2's highest upscale, for very powerful graphics cards.
      .concat([['8K', '12x  7680 x 5376']]);""")

if guide_ids:
    for key, cap, label, _ in GUIDES:
        pass
    old_sl = re.search(r"    const SL = \[\n(.*?)\n    \];", logic, re.S).group(0)
    new_sl = "    const SL = [\n" + ",\n".join(
        "      { src: '%s', cap: %s, label: %s }" % (guide_ids[k], json.dumps(c), json.dumps(l)) for k, c, l, _ in GUIDES) + "\n    ];"
    L(old_sl, new_sl)

# Scarf colour row (Mod Menu).
L("""      modRows: modRows,""",
  """      modRows: modRows,
      scarfDesc: s.scarf ? 'Hotsuma\\'s scarf in ' + s.scarf.toUpperCase() + '. Save to keep it; it changes in game straight away.' : 'Hotsuma\\'s scarf, red as in the original game. Pick a swatch or any colour.',
      scarfHex: s.scarf || '#b0161e',
      scarfSwatches: [['Original red', ''], ['Blue', '#2456d6'], ['Purple', '#7b35c9'], ['Green', '#2b9a45'],
                      ['Gold', '#d4a017'], ['White', '#ececec'], ['Black', '#1c1c1f'], ['Pink', '#e2559e']].map((c) => ({
        name: c[0], on: (s.scarf || '') === c[1],
        style: 'width:22px;height:22px;border-radius:50%;padding:0;cursor:pointer;background:' + (c[1] || '#b0161e') + ';border:' +
               ((s.scarf || '') === c[1] ? '3px solid #1b1b1f;box-shadow:0 0 0 2px #b8122b' : '2px solid rgba(0,0,0,.25)'),
        pick: () => this.setState({ scarf: c[1] })
      })),
      scarfPick: (e) => {
        const v = String(e.target.value || '').toLowerCase();
        if (/^#[0-9a-f]{6}$/.test(v)) this.setState({ scarf: v === '#b0161e' ? '' : v });
      },""")

# Mods this port has.
L("""    const MODS = [
      ['intro', 'Skip intro movies', 'Go straight to the title screen.'],
      ['fps', 'Unlocked frame rate', 'Run above the original 60 Hz cap.'],
      ['tex', 'Custom texture pack', 'Load replacement textures from the mods folder.'],
      ['dbg', 'Debug overlay', 'Show frame time and renderer info in-game.']
    ];""",
  """    const MODS = [
      ['studio', 'Twilight Orchard intro', 'Our studio\\'s opening before the game starts. Any key or button skips it.'],
      ['intro', 'Skip start-up logos', 'SEGA, Overworks and ADX logos pass in a second each. Start, Cross or Enter during them does the same.'],
      ['fastvu', 'Fast VU maths', 'A little faster; rare results can differ from the PS2.'],
      ['dbg', 'Debug overlay', 'Show the frame rate in-game.'],
      ['diag', 'Record diagnostics', 'Save a performance profile and sound trace (game/captures).'],
      ['legacy', 'Legacy sound chip', 'The older built-in sound model instead of PCSX2\\'s SPU2.'],
      ['nscale', 'Effects at high resolution', 'Blur, glow and fog drawn at the PS2\\'s size, then scaled (PCSX2 native scaling).'],
      ['rr5fix', 'Ridge Racer blur fix', 'A bridge fix made for Ridge Racer V. Off for Shinobi; on only to compare.'],
      ['dropst', 'Drop stretched triangles', 'Ridge Racer V\\'s fix. Off draws every effect layer, as PCSX2 does.'],
      ['record', 'Record drawing (effects check)', 'Saves every drawing command from boot (several GB, game/captures).']
    ];""")

# Launch: the real game.
L("""    const status = p < 25 ? 'Verifying game image...' : p < 55 ? 'Loading recompiled game...' : p < 80 ? 'Building shader cache...' : 'Starting Shinobi...';""",
  """    const status = 'Starting Shinobi...';""")
L("""      play: () => {
        const st = this.state;
        if (st.launch === 'launching') return;
        if (st.launch === 'running') { this.setState({ launch: 'idle', progress: 0 }); this.toastMsg('Game stopped'); return; }
        clearInterval(this.timer);
        this.setState({ launch: 'launching', progress: 0 });
        this.timer = setInterval(() => {
          const n = this.state.progress + 2;
          if (n >= 100) { clearInterval(this.timer); this.setState({ launch: 'running', progress: 100 }); }
          else this.setState({ progress: n });
        }, 70);
      },""",
  """      play: () => {
        if (this.ingame) { if (this.host) this.host.postMessage('resume'); return; }
        const st = this.state;
        if (st.launch === 'launching') return;
        if (st.launch === 'running') { this.send('stop'); return; }
        clearInterval(this.timer);
        this.setState({ launch: 'launching', progress: 5 });
        // The bar only shows that the game is starting; the host reports when its window is up.
        this.timer = setInterval(() => {
          const n = Math.min(this.state.progress + 3, 90);
          this.setState({ progress: n });
        }, 120);
        this.send('play');
      },""")
L("""      playLabel: launching ? status : running ? 'Stop Game' : 'Play Game (Recom)',""",
  """      playLabel: this.ingame ? 'Resume Game' : launching ? status : running ? 'Stop Game' : 'Play Game (Recom)',""")
L("""      chipText: running ? 'Running - ' + s.renderer + ' - ' + s.res : status + ' ' + p + '%',""",
  """      chipText: running ? 'Running - ' + s.renderer + ' - ' + s.res + (s.ws ? ' - ' + s.wsShape : '') : status,""")
L("""      rendererOpts: seg(['OpenGL', 'Vulkan', 'D3D12'], 'renderer', s.renderer),""",
  """      rendererOpts: seg(['D3D11', 'Vulkan'], 'renderer', s.renderer),
      fpsOpts: seg(['30', '50', '60', 'Unlocked'], 'fps', s.fps).map((o) => Object.assign(o, {
        cls: o.cls + (o.name === 'Unlocked' ? ' wide' : ''),
        tip: { '30': 'A steady, evenly paced 30', '50': '5 of every 6 frames at normal speed (for 50 Hz screens)',
               '60': "The game's own rate", 'Unlocked': 'No frame cap and no V-Sync wait (lowest lag, may tear)' }[o.name] })),
      wsName: s.wsShape,
      wsOpts: ['16:9', '21:9', '32:9', 'Auto'].map((n) => ({ name: n, on: s.wsShape === n, cls: s.wsShape === n ? 'on' : '', pick: set({ wsShape: n }) })),""")
L("""      texName: QUAL[s.tex],
      fxName: QUAL[s.fx],""",
  """      texName: ['Off', '2x', '4x', '8x', '16x'][s.tex],
      fxName: ['Off', 'Low', 'Medium', 'High', 'Max'][s.fx],""")
L("""      aaOpts: seg(['Off', 'FXAA', 'MSAA'], 'aa', s.aa),
      showMsaa: s.aa === 'MSAA',""",
  """      aaOpts: seg(['Off', 'FXAA', 'Edge AA'], 'aa', s.aa),
      showMsaa: false,""")
L("""      save: () => {
        if (this.sig(this.state) === this.state.saved) { this.toastMsg('No changes to save'); return; }
        this.setState({ saved: this.sig(this.state) });
        this.toastMsg('Settings saved');
      },""",
  """      save: () => {
        if (this.sig(this.state) === this.state.saved) { this.toastMsg('No changes to save'); return; }
        this.send('save');
      },""")
L("""      openModsFolder: () => this.toastMsg('Opening mods folder...'),""",
  """      openModsFolder: () => { this.send('openfolder'); this.toastMsg('Opening the game folder...'); },""")
L("""      hasToast: !!s.toast,""",
  """      toggleMute: () => {
        const muted = !this.state.muted;
        this.setState({ muted });
        document.documentElement.classList.toggle('muted', muted);
        if (this.music) {
          this.music.muted = muted;
          if (!muted && this.state.launch === 'idle') this.musicOn();
        }
        if (this.host) this.host.postMessage('mute\\nmuted=' + (muted ? 1 : 0));
      },
      winMin: () => { if (this.host) this.host.postMessage('minimize'); },
      winMax: () => { if (this.host) this.host.postMessage('maximize'); },
      winClose: () => { if (this.host) this.host.postMessage(this.ingame ? 'resume' : 'close'); },
      titleText: this.ingame ? 'Shinobi (PS2 Recom) - Paused' : 'Shinobi (PS2 Recom) Launcher - by TheRealAfsan',
      cardTitle: this.ingame ? 'Game paused' : 'Shinobi - PS2 Recompilation',
      cardText: this.ingame
        ? 'Change settings while you play. Most apply at once; resolution, renderer and effect switches after a restart. '
          + 'Controller: D-pad to move, Cross to choose, Circle to go back, Start to resume. Keyboard: arrows, Enter, Esc, F1.'
        : 'Ignite the fiery vengeance of Hotsuma, the last of the Oboro clan. Scale walls and slash your way through the '
          + 'demon-filled streets of post-apocalyptic Tokyo. But beware, you\\'ll have to keep your bloodthirsty sword '
          + 'satisfied or suffer its deadly curse yourself.',
      lastLabel: !this.ingame ? 'Credits' : s.quitArmed ? 'Press again to quit' : 'Quit Game',
      lastCls: this.ingame && s.quitArmed ? 'warn' : '',
      lastBtn: () => {
        if (!this.ingame) { this.setState({ modal: 'credits' }); return; }
        if (this.state.quitArmed) { if (this.host) this.host.postMessage('quit'); return; }
        this.setState({ quitArmed: true });
        clearTimeout(this.quitTimer);
        this.quitTimer = setTimeout(() => this.setState({ quitArmed: false }), 3000);
      },
      resetLabel: this.ingame && s.restart ? 'Restart Game' : 'Reset to Defaults',
      resetCls: this.ingame && s.restart ? 'warn' : '',
      resetBtn: () => {
        if (this.ingame && this.state.restart) { this.send('restart'); return; }
        this.setState(Object.assign({ resOpen: false }, this.defaults));
        this.toastMsg('Defaults restored - save to keep them');
      },
      // Lighter settings for slower PCs: the game's own resolution, no extra texture filtering, sharpening or
      // anti-aliasing, PCSX2's lightest blending accuracy; frame rate, widescreen and V-Sync as the defaults.
      lowEndBtn: () => {
        this.setState(Object.assign({ resOpen: false }, this.defaults, { res: 'Native', tex: 0, fx: 0, aa: 'Off', blend: 'minimum' }));
        this.toastMsg('Low-end PC settings - save to keep them');
      },

      hasToast: !!s.toast,""")
# Game file: state, host messages, dialog, play guard.
L("""    this.state.restart = !!(init && init.restart);""",
  """    this.state.restart = !!(init && init.restart);
    // Scarf colour: '' = the original red, else '#rrggbb'.
    this.state.scarf = (init && init.settings && init.settings.scarf) || '';
    this.state.iso = (init && init.iso) || '';
    this.state.installed = init ? !!init.installed : true;
    this.state.isoMsg = '';
    this.state.isoFound = init ? !!init.isoFound : true;
    this.state.savesDir = (init && init.savesDir) || '';
    this.state.installPct = -1;
    this.state.installFile = '';""")
L("""    else if (m.indexOf('error:') === 0) {""",
  """    else if (m.indexOf('iso:') === 0) { const a = m.slice(4).split('|'); this.setState({ iso: a[0] === 'ok' ? (a[1] || '') : this.state.iso, isoFound: a[0] === 'ok' ? true : this.state.isoFound, isoMsg: a.slice(2).join('|') }); }
    else if (m.indexOf('install:') === 0) { const a = m.slice(8).split('|'); this.setState({ installPct: parseInt(a[0], 10) || 0, installFile: a.slice(1).join('|') }); }
    else if (m === 'installed') { this.setState({ installed: true, installPct: -1, installFile: '', isoMsg: 'The game files are set up. You can close this and press Play.' }); this.toastMsg('Game files set up'); }
    else if (m.indexOf('install-error:') === 0) { this.setState({ installPct: -1, installFile: '', isoMsg: m.slice(14) }); this.toastMsg(m.slice(14)); }
    else if (m.indexOf('error:') === 0) {""")
L("""    const modalTitles = { controller: 'Configure Controller', mods: 'Mod Menu', credits: 'Credits', guides: 'Guides' };""",
  """    const modalTitles = { controller: 'Configure Controller', mods: 'Mod Menu', credits: 'Credits', guides: 'Guides', game: 'Game File' };""")
L("""        if (st.launch === 'launching') return;
        if (st.launch === 'running') { this.send('stop'); return; }""",
  """        if (!st.installed) { this.setState({ modal: 'game' }); return; }
        if (st.launch === 'launching') return;
        if (st.launch === 'running') { this.send('stop'); return; }""")
L("""      playLabel: this.ingame ? 'Resume Game' : launching ? status : running ? 'Stop Game' : 'Play Game (Recom)',""",
  """      playLabel: this.ingame ? 'Resume Game' : !s.installed ? 'Select Game File' : launching ? status : running ? 'Stop Game' : 'Play Game (Recom)',""")
L("""openMods: set({ modal: 'mods' }),""",
  """openMods: set({ modal: 'mods' }),
      openGame: set({ modal: 'game' }),
      gameBtnLabel: s.installed ? 'Game File' : 'Select Game File',
      gameBtnCls: s.installed ? '' : 'warn',
      gameCaption: s.iso
        ? (s.isoFound ? 'Game file: ' : 'Not found: ') + ((p, bs) => p.length > 38 ? p.slice(0, 3) + '...' + bs + p.split(bs).pop() : p)(s.iso, String.fromCharCode(92))
        : s.installed ? 'Game files are set up in the game folder' : 'No game file chosen yet',
      gfileCls: (s.iso && !s.isoFound) || (!s.iso && !s.installed) ? 'warn' : '',
      showGame: s.modal === 'game',
      gameIntro: 'This launcher does not contain the game. Choose your own copy of Shinobi (PlayStation 2, USA) as a disc image '
        + '(.iso, or .bin / .img). The launcher checks that it is the right game and copies its files into the game folder.',
      isoPath: s.iso || 'No file chosen',
      isoStatus: s.isoMsg || (s.installed ? 'The game files are set up.' : s.iso ? 'Press Set up to copy the game files.' : 'No game file chosen yet.'),
      installBusy: s.installPct >= 0,
      installFill: Math.max(s.installPct, 0) + '%',
      installLabel: s.installPct >= 0 ? 'Setting up... ' + s.installPct + '%' : s.installed ? 'Check / set up again' : 'Set up game files',
      installNote: s.installPct >= 0 ? (s.installFile || 'Copying...') : 'Setting up copies several GB the first time. Files already copied are skipped.',
      browseIso: () => { if (this.host) this.host.postMessage('browseiso'); },
      savesPath: (this.state.savesDir || 'saves'),
      openSaves: () => { if (this.host) this.host.postMessage('opensaves'); },
      installIso: () => {
        if (!this.state.iso) { this.toastMsg('Choose your disc image first (Browse...)'); return; }
        if (!this.ownedNow()) { this.toastMsg('Tick that you own this copy of the game first'); return; }
        this.setState({ installPct: 0, installFile: '', isoMsg: '' }); if (this.host) this.host.postMessage('installiso');
      },
      owned: this.ownedNow(),
      openManual: () => { if (this.host) this.host.postMessage('manual'); else this.toastMsg('Launcher host not connected'); },
      toggleOwned: () => {
        const owned = !this.ownedNow();
        try { localStorage.setItem('shinobi.owned', owned ? '1' : '0'); } catch (e) {}
        this.setState({ ownedTick: owned });
      },
      gameSteps: ((done) => [
        ['Disc image chosen', !!s.iso && s.isoFound],
        ['Checked: Shinobi (USA, SLUS-20459)', !!s.iso && s.isoFound && !/not|wrong|cannot|could/i.test(s.isoMsg || '')],
        ['Game files set up', !!s.installed]
      ].map((st) => ({
        label: st[0], icon: st[1] ? '\u2713' : '\u25CB',
        mark: 'display:inline-block;width:18px;text-align:center;font-weight:700;color:' + (st[1] ? '#1f8a3b' : '#9a9aa2'),
        text: 'font-weight:600;color:' + (st[1] ? '#1b1b1f' : '#7a7a82')
      })))(),""")


# The "I own this copy" tick: remembered, and taken as given once the game files are set up.
L("""  toastMsg(msg) {""",
  """  // The game manual as a book: pages drawn from the player's own PDF (game/manual), turned with StPageFlip like the
  // CS World reader: drag a page by its corner to lift, fold and turn it, or use the arrows, the arrow keys or a swipe.
  manualLib() {
    if (window.St && window.St.PageFlip) return Promise.resolve();
    if (!this._flipLib) this._flipLib = new Promise((res, rej) => {
      const sc = document.createElement('script'); sc.src = 'page-flip.browser.js';
      sc.onload = res; sc.onerror = () => { this._flipLib = null; rej(new Error('page-flip.browser.js is missing')); };
      document.head.appendChild(sc);
    });
    return this._flipLib;
  }

  // One page-turn back, animated like Next (the corner-only rule is lifted for the request).
  _bookBack(book) {
    const i = book.getCurrentPageIndex();
    if (i <= 0) return;
    const set = book.getSettings();
    const was = set.disableFlipByClick;
    set.disableFlipByClick = false;
    book.flipPrev('top');
    setTimeout(() => {
      set.disableFlipByClick = was;
      // Still not moved (the library refused): no animation, but at least the page turns.
      if (book.getCurrentPageIndex() === i && book.getState && book.getState() === 'read') book.turnToPage(Math.max(0, i - 2));
    }, 900);
  }

  manualClose() {
    if (this.manualOnly && this._bookEl && !this._manualHiding) { this.manualHide(); return; }
    this._manualTeardown();
  }

  // F4 in game: the manual slides down, then the game resumes.
  manualHide() {
    if (this._manualHiding) return;
    this._manualHiding = true;
    const el = this._bookEl;
    if (el) { el.style.transition = 'transform .3s cubic-bezier(.5,0,.75,0)'; el.style.transform = 'translateY(100%)'; }
    setTimeout(() => {
      this._manualHiding = false;
      this._manualTeardown();
      if (this.host) this.host.postMessage('resume');
    }, el ? 300 : 0);
  }

  _manualTeardown() {
    if (this._book) { try { this._book.destroy(); } catch (e) {} this._book = null; }
    if (this._bookEl) { this._bookEl.remove(); this._bookEl = null; }
    document.documentElement.classList.remove('manual-open');
    if (this._bookKeys) { window.removeEventListener('keydown', this._bookKeys, true); this._bookKeys = null; }
  }

  manualShow(count, base, stamp) {
    if (!count) { this.toastMsg('The manual has no pages'); return; }
    this.manualClose();
    const pad = (n) => String(n).padStart(3, '0');
    const urls = []; for (let i = 1; i <= count; i++) urls.push(base + pad(i) + '.png' + (stamp ? '?v=' + stamp : ''));
    const el = document.createElement('div');
    el.setAttribute('role', 'dialog'); el.setAttribute('aria-label', 'Game manual');
    // The launcher's title bar is a window drag area (app-region); the manual covers it and must not move the window.
    el.style.cssText = 'position:fixed;inset:0;z-index:99999;background:rgba(14,14,17,.94);display:flex;flex-direction:column;'
      + 'font-family:Rajdhani,sans-serif;color:#eee;user-select:none;app-region:no-drag;-webkit-app-region:no-drag';
    const btn = 'background:#2a2a30;color:#fff;border:1px solid #55555e;border-radius:4px;padding:6px 14px;font:600 14px Rajdhani,sans-serif;'
      + 'letter-spacing:.06em;text-transform:uppercase;cursor:pointer';
    if (this.manualOnly) {
      el.style.transform = 'translateY(100%)';
      requestAnimationFrame(() => requestAnimationFrame(() => { el.style.transition = 'transform .38s cubic-bezier(.2,.8,.2,1)'; el.style.transform = 'none'; }));
    }
    el.innerHTML =
      '<div style="display:flex;align-items:center;gap:12px;padding:10px 16px;border-bottom:2px solid #b3122a;background:#1b1b1f;'
      + 'app-region:no-drag;-webkit-app-region:no-drag">'
      + '<b style="font-size:18px;letter-spacing:.08em;text-transform:uppercase">Shinobi \u2013 Game Manual</b>'
      + '<span class="mpage" style="margin-left:6px;color:#aaa;font-size:14px"></span><span style="flex:1"></span>'
      + (this.manualOnly ? '<span style="color:#aaa;font-size:14px;letter-spacing:.06em">F4 TO CLOSE</span>'
         : '<button class="mpick" style="' + btn + '" title="Use a different PDF">Choose another PDF</button>'
         + '<button class="mclose" style="' + btn + ';padding:6px 12px" aria-label="Close">\u2715</button>') + '</div>'
      + '<div class="mstage" style="flex:1;min-height:0;display:flex;align-items:center;justify-content:center;padding:14px 56px;position:relative;overflow:hidden">'
      + '<div class="mflip" style="width:100%;height:100%"></div>'
      + '<div class="mzview" style="position:absolute;inset:0;display:none;cursor:grab;z-index:5;overflow:hidden;background:rgba(14,14,17,.98)">'
      + '<div class="mzimgs" style="position:absolute;left:0;top:0;display:flex;transform-origin:0 0;will-change:transform"></div></div>'
      + '<div class="mzhint" style="position:absolute;right:14px;bottom:8px;font-size:12px;color:#888;pointer-events:none">'
      + 'Mouse wheel: zoom \u00b7 Drag a page corner: turn it \u00b7 Double-click: fit</div></div>'
      + '<div style="display:flex;justify-content:center;gap:14px;padding:10px 0 14px">'
      + '<button class="mprev" style="' + btn + '">\u2039 Previous</button><button class="mnext" style="' + btn + '">Next \u203A</button></div>';
    document.body.appendChild(el);
    document.documentElement.classList.add('manual-open');
    this._bookEl = el;
    const pageText = el.querySelector('.mpage');
    if (el.querySelector('.mclose')) el.querySelector('.mclose').onclick = () => this.manualClose();
    if (el.querySelector('.mpick')) el.querySelector('.mpick').onclick = () => { this.manualClose(); if (this.host) this.host.postMessage('manualpick'); };
    this._bookKeys = (e) => {
      if (!this._book) return;
      if (e.key === 'Escape') { e.preventDefault(); e.stopPropagation(); this.manualClose(); }
      else if (e.key === 'ArrowRight' || e.key === 'PageDown') { e.preventDefault(); e.stopPropagation(); if (this._bookFit) this._bookFit(); this._book.flipNext('top'); }
      else if (e.key === 'ArrowLeft' || e.key === 'PageUp') { e.preventDefault(); e.stopPropagation(); if (this._bookFit) this._bookFit(); this._bookBack(this._book); }
    };
    window.addEventListener('keydown', this._bookKeys, true);
    const first = new Image();
    first.onload = () => {
      this.manualLib().then(() => {
        if (this._bookEl !== el) return;
        const ratio = first.naturalWidth / Math.max(1, first.naturalHeight);
        const host = el.querySelector('.mflip');
        // The CS World reader's settings: a page turns only when it is dragged (or by the arrows, keys or a swipe).
        const book = new window.St.PageFlip(host, {
          width: Math.round(1000 * ratio), height: 1000, size: 'stretch', minWidth: 100, maxWidth: 3000, minHeight: 150, maxHeight: 3000,
          showCover: true, usePortrait: true, autoSize: true, drawShadow: true, maxShadowOpacity: 0.45, flippingTime: 650,
          mobileScrollSupport: false, showPageCorners: false, disableFlipByClick: true, swipeDistance: 24, clickEventForward: false
        });
        book.loadFromImages(urls);
        // Zoom with the mouse wheel, about the point under the cursor (1x to 4x). Zoomed in, a still view of the pages
        // that are open (sharp, from the full-size page images) is shown instead of the book: drag moves it around, only
        // as far as the pages' own edges, and smaller than the view it stays centred. Zooming back out to 1x (or a
        // double-click, a turn by button or key) returns to the book.
        const stage = el.querySelector('.mstage'), zv = el.querySelector('.mzview'), zi = el.querySelector('.mzimgs');
        const z = { s: 1, x: 0, y: 0, w: 0, h: 0 };
        const openPages = () => {
          const i = book.getCurrentPageIndex(), n = urls.length;
          if (!(book.getOrientation && book.getOrientation() === 'landscape')) return [i];
          if (i === 0) return [0];
          const left = i % 2 === 1 ? i : i - 1;
          return left + 1 < n ? [left, left + 1] : [left];
        };
        const layout = () => {
          // At 1x the still view has the book's size and place: the page images at the stage's inner height.
          const sr = stage.getBoundingClientRect(), cs = getComputedStyle(stage);
          const pt = parseFloat(cs.paddingTop) || 0, pb = parseFloat(cs.paddingBottom) || 0;
          const h = Math.max(50, sr.height - pt - pb), ratio = first.naturalWidth / Math.max(1, first.naturalHeight);
          zi.querySelectorAll('img').forEach((im) => { im.style.height = h + 'px'; im.style.width = Math.round(h * ratio) + 'px'; });
          z.w = zi.children.length * Math.round(h * ratio); z.h = h;
          return { vw: sr.width, vh: sr.height };
        };
        const clamp = (v) => {
          const axis = (pos, size, view) => size <= view ? (view - size) / 2 : Math.min(0, Math.max(view - size, pos));
          z.x = axis(z.x, z.w * z.s, v.vw);
          z.y = axis(z.y, z.h * z.s, v.vh);
        };
        const apply = () => {
          if (z.s <= 1) { z.s = 1; zv.style.display = 'none'; zi.innerHTML = ''; return; }
          if (zv.style.display !== 'block') {
            zi.innerHTML = openPages().map((k) => '<img draggable="false" alt="" src="' + urls[k] + '" style="display:block;user-select:none">').join('');
            zv.style.display = 'block';
          }
          clamp(layout());
          zi.style.transform = 'translate(' + z.x + 'px,' + z.y + 'px) scale(' + z.s + ')';
        };
        const zoomAt = (cx, cy, next) => {
          next = Math.max(1, Math.min(4, next));
          if (next === 1) { z.s = 1; apply(); return; }
          const r = stage.getBoundingClientRect(), px = cx - r.left, py = cy - r.top;
          if (z.s === 1) {
            // starting to zoom: the still view starts exactly where the book is
            const v = layout(); z.x = (v.vw - z.w) / 2; z.y = (v.vh - z.h) / 2;
          }
          z.x = px - (px - z.x) * (next / z.s); z.y = py - (py - z.y) * (next / z.s); z.s = next; apply();
        };
        stage.addEventListener('wheel', (e) => { e.preventDefault(); zoomAt(e.clientX, e.clientY, z.s * (e.deltaY < 0 ? 1.15 : 1 / 1.15)); }, { passive: false });
        stage.addEventListener('dblclick', () => { z.s = 1; apply(); });
        let drag = null;
        zv.addEventListener('pointerdown', (e) => { drag = { x: e.clientX - z.x, y: e.clientY - z.y }; try { zv.setPointerCapture(e.pointerId); } catch (_) {} zv.style.cursor = 'grabbing'; });
        zv.addEventListener('pointermove', (e) => { if (!drag) return; z.x = e.clientX - drag.x; z.y = e.clientY - drag.y; apply(); });
        const end = () => { drag = null; zv.style.cursor = 'grab'; };
        zv.addEventListener('pointerup', end); zv.addEventListener('pointercancel', end);
        window.addEventListener('resize', () => { if (z.s > 1) apply(); });
        // Turning a page with the buttons or keys goes back to the book first, so the turn is seen whole.
        const fit = () => { if (z.s !== 1) { z.s = 1; apply(); } };
        this._bookFit = fit;
        const show = () => { const i = book.getCurrentPageIndex(); pageText.textContent = 'Page ' + (i + 1) + ' of ' + count; };
        book.on('flip', show); show();
        el.querySelector('.mprev').onclick = () => { fit(); this._bookBack(book); };
        el.querySelector('.mnext').onclick = () => { fit(); book.flipNext('top'); };
        this._book = book; window.__manualBook = book;
      }).catch((e) => { this.toastMsg(String(e && e.message || e)); this.manualClose(); });
    };
    first.onerror = () => { this.toastMsg('The manual pages could not be loaded'); this.manualClose(); };
    first.src = urls[0];
  }

  ownedNow() {
    if (typeof this.state.ownedTick === 'boolean') return this.state.ownedTick;
    let stored = null;
    try { stored = localStorage.getItem('shinobi.owned'); } catch (e) {}
    return stored === '1' || (stored === null && !!this.state.installed);
  }

  toastMsg(msg) {""")

# Camera (the pad's right stick) on the keyboard: four more rows in Configure Controller, defaults I K J L.
L("""['start', 'Start', 'Menu'], ['select', 'Select', 'View']];""",
  """['start', 'Start', 'Menu'], ['select', 'Select', 'View'],
      ['camera_up', 'Camera Up', 'Right stick up'], ['camera_down', 'Camera Down', 'Right stick down'],
      ['camera_left', 'Camera Left', 'Right stick left'], ['camera_right', 'Camera Right', 'Right stick right']];""")
L("""start: 'ENTER', select: 'TAB' };""",
  """start: 'ENTER', select: 'TAB', camera_up: 'I', camera_down: 'K', camera_left: 'J', camera_right: 'L' };""")

# Public page: the game's back-of-box text is not used.
if PUBLIC:
    L(r"""        : 'Ignite the fiery vengeance of Hotsuma, the last of the Oboro clan. Scale walls and slash your way through the '
          + 'demon-filled streets of post-apocalyptic Tokyo. But beware, you\'ll have to keep your bloodthirsty sword '
          + 'satisfied or suffer its deadly curse yourself.',""",
      r"""        : 'A native Windows port of Shinobi (PlayStation 2, USA) by TheRealAfsan. Choose your own copy of the game under '
          + 'Game File, set up its files once, then press Play. This program contains no game files.',""")

# The Mod Menu's footer button opens the game folder.
template = must_replace(template, '<span class="lab">Open Mods Folder</span>', '<span class="lab">Open Game Folder</span>')




# ---- LOCAL PATCH: tabs, window mode, haptics (page state, saving, bindings) ----
L("""blend: 'basic' };
    this.defBinds""", """blend: 'basic', win: 'borderless', haptics: true, hapStr: 10, trig: true };
    this.defBinds""")
L("""      resOpen: false, modal: null, guide: 0,
      launch: 'idle'""", """      resOpen: false, winOpen: false, tab: 'display', modal: null, guide: 0,
      launch: 'idle'""")
L("""blend: s.blend, scarf: s.scarf || '', bind: s.bind, mods: s.mods });""",
  """blend: s.blend, scarf: s.scarf || '', win: s.win, haptics: s.haptics, hapStr: s.hapStr, trig: s.trig, bind: s.bind, mods: s.mods });""")
L("""    add('scarf', s.scarf || 'original');""", """    add('scarf', s.scarf || 'original');
    add('win', s.win || 'borderless'); add('haptics', s.haptics ? 1 : 0); add('hapStr', s.hapStr || 10); add('trig', s.trig ? 1 : 0);""")
L("""      setTex: (e) => this.setState({ tex: Number(e.target.value) }),""",
  """      setTex: (e) => this.setState({ tex: Number(e.target.value) }),
      tabOpts: [['display', 'Display'], ['graphics', 'Graphics'], ['controller', 'Controller']].map((t) => ({ name: t[1], on: s.tab === t[0], cls: s.tab === t[0] ? 'on' : '', pick: () => { document.documentElement.classList.add('tabbed'); this.setState({ tab: t[0], resOpen: false, winOpen: false }); } })),
      tabDisplay: s.tab === 'display',
      tabGraphics: s.tab === 'graphics',
      tabController: s.tab === 'controller',
      winOpen: s.winOpen,
      winLabel: ({ windowed: 'Windowed', borderless: 'Borderless Fullscreen', fullscreen: 'Exclusive Fullscreen' })[s.win || 'borderless'],
      toggleWin: () => this.setState({ winOpen: !this.state.winOpen, resOpen: false }),
      closeWin: () => this.setState({ winOpen: false }),
      winOptions: [['borderless', 'Borderless Fullscreen', 'fills the screen, quick to alt-tab'], ['fullscreen', 'Exclusive Fullscreen', 'the screen is the game\\'s alone'], ['windowed', 'Windowed', 'a window you can move and resize']].map((w) => ({ name: w[1], sub: w[2], on: (s.win || 'borderless') === w[0], cls: (s.win || 'borderless') === w[0] ? 'on' : '', pick: () => this.setState({ win: w[0], winOpen: false }) })),
      haptics: s.haptics !== false,
      toggleHaptics: () => this.setState({ haptics: !(this.state.haptics !== false) }),
      hapStr: s.hapStr || 10,
      hapStrName: ((s.hapStr || 10) * 10) + '%',
      hapStrBg: fill(((s.hapStr || 10) - 1) / 9 * 4),
      setHapStr: (e) => this.setState({ hapStr: Number(e.target.value) }),
      trig: s.trig !== false,
      toggleTrig: () => this.setState({ trig: !(this.state.trig !== false) }),""")


# ---- LOCAL PATCH: Effects tab state, saving and bindings ----
L("""win: 'borderless', haptics: true, hapStr: 10, trig: true };""",
  """win: 'borderless', haptics: true, hapStr: 10, trig: true, texf: 'ps2', scrf: 'smooth', dith: true, mip: true, upfix: true, ovl: false, cam: false };""")
L("""win: s.win, haptics: s.haptics, hapStr: s.hapStr, trig: s.trig, bind: s.bind""",
  """win: s.win, haptics: s.haptics, hapStr: s.hapStr, trig: s.trig, texf: s.texf, scrf: s.scrf, dith: s.dith, mip: s.mip, upfix: s.upfix, ovl: s.ovl, cam: s.cam, bind: s.bind""")
L("""add('trig', s.trig ? 1 : 0);""",
  """add('trig', s.trig ? 1 : 0);
    add('texf', s.texf || 'ps2'); add('scrf', s.scrf || 'smooth'); add('dith', s.dith !== false ? 1 : 0); add('mip', s.mip !== false ? 1 : 0); add('upfix', s.upfix !== false ? 1 : 0); add('ovl', s.ovl ? 1 : 0); add('cam', s.cam ? 1 : 0);""")
L("""[['display', 'Display'], ['graphics', 'Graphics'], ['controller', 'Controller']]""",
  """[['display', 'Display'], ['graphics', 'Graphics'], ['effects', 'Effects'], ['controller', 'Controller']]""")
L("""      tabController: s.tab === 'controller',""",
  """      tabController: s.tab === 'controller',
      tabEffects: s.tab === 'effects',
      blendOpts: [['minimum', 'Min'], ['basic', 'Basic'], ['medium', 'Medium'], ['high', 'High']].map((o) => ({ name: o[1], on: (s.blend || 'basic') === o[0], cls: (s.blend || 'basic') === o[0] ? 'on' : '', pick: () => this.setState({ blend: o[0] }) })),
      texfOpts: [['ps2', 'As on PS2'], ['bilinear', 'Bilinear']].map((o) => ({ name: o[1], on: (s.texf || 'ps2') === o[0], cls: (s.texf || 'ps2') === o[0] ? 'on' : '', pick: () => this.setState({ texf: o[0] }) })),
      scrfOpts: [['sharp', 'Sharp'], ['smooth', 'Smooth'], ['scanlines', 'Scanlines'], ['crt', 'CRT']].map((o) => ({ name: o[1], on: (s.scrf || 'smooth') === o[0], cls: (s.scrf || 'smooth') === o[0] ? 'on' : '', pick: () => this.setState({ scrf: o[0] }) })),
      dith: s.dith !== false,
      toggleDith: () => this.setState({ dith: !(this.state.dith !== false) }),
      mip: s.mip !== false,
      toggleMip: () => this.setState({ mip: !(this.state.mip !== false) }),
      upfix: s.upfix !== false,
      toggleUpfix: () => this.setState({ upfix: !(this.state.upfix !== false) }),
      ovl: !!s.ovl,
      toggleOvl: () => this.setState({ ovl: !this.state.ovl }),
      cam: !!s.cam,
      toggleCam: () => this.setState({ cam: !this.state.cam }),""")

template = template[:logic_start] + logic + template[logic_end:]
set_block('manifest', json.dumps(manifest, separators=(',', ':')))
set_block('template', json.dumps(template).replace('</', r'<\u002F'))  # keep </script> inside the block escaped
os.makedirs(os.path.dirname(out_path), exist_ok=True)
open(out_path, 'w', encoding='utf-8').write(page)
print('wrote', out_path, len(page), 'chars;', len(guide_ids), 'guide pictures')
