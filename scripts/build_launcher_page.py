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
                        '<button class="btn {{lastCls}}" sc-camel-on-click="{{lastBtn}}"><span class="lab">{{lastLabel}}</span></button>')
template = must_replace(template, '<button class="btn sm" sc-camel-on-click="{{reset}}"><span class="lab">Reset to Defaults</span></button>',
                        '<button class="btn sm {{resetCls}}" sc-camel-on-click="{{resetBtn}}"><span class="lab">{{resetLabel}}</span></button>')

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
                        '                <div class="pathrow"><span class="pathtxt" title="{{isoPath}}">{{isoPath}}</span>'
                        '<button class="btn sm" sc-camel-on-click="{{browseIso}}"><span class="lab">Browse...</span></button></div>\n'
                        '                <p class="note">{{isoStatus}}</p>\n'
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
    this.defaults = { res: '1080p', renderer: 'D3D11', fps: '60', ws: true, wsShape: '16:9', vsync: true, tex: 1, fx: 1, aa: 'Off', msaa: 4 };
    this.defBinds = { up: 'UP,W', down: 'DOWN,S', left: 'LEFT,A', right: 'RIGHT,D', cross: 'X,SPACE', circle: 'C,ESCAPE', square: 'Z,KP_0', triangle: 'V,KP_1', l1: 'Q', r1: 'E', l2: 'LEFT_SHIFT', r2: 'RIGHT_SHIFT', start: 'ENTER', select: 'TAB' };
    this.defMods = { studio: true, nscale: true, rr5fix: false, dropst: false, record: false, intro: false, fastvu: false, dbg: false, full: true, diag: false, legacy: false };
    const init = window.__launcherInit || null;
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
    const scope = document.querySelector('.dlg') || document;
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
    el.focus({ preventScroll: true });
    target.scrollIntoView({ block: 'nearest' });
  }

  navMove(delta) {
    const items = this.navItems();
    if (!items.length) return;
    let i = items.indexOf(this.navCurrent);
    i = i < 0 ? 0 : (i + delta + items.length) % items.length;
    this.navFocus(items[i]);
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
      const next = sibs[sibs.indexOf(el) + delta];
      if (next) { next.click(); setTimeout(() => this.navFocus(next), 0); }
      return;
    }
    this.navMove(delta);
  }

  navBack() {
    const x = document.querySelector('.dlg .x');
    if (x) { x.click(); setTimeout(() => this.navMove(0), 0); return; }
    if (this.state.resOpen) { this.setState({ resOpen: false }); return; }
    if (this.ingame && this.host) this.host.postMessage('resume');
  }

  navPress() {
    const el = this.navCurrent;
    if (!el || !document.contains(el)) { this.navMove(0); return; }
    el.click();
    setTimeout(() => { if (!document.contains(this.navCurrent)) this.navMove(0); }, 0);
  }

  navSetup() {
    document.addEventListener('keydown', (e) => {
      if (this.state && this.state.listening) return; // binding a key in Configure Controller
      const k = e.key;
      if (k === 'ArrowDown' || k === 'ArrowUp') { e.preventDefault(); this.navMove(k === 'ArrowDown' ? 1 : -1); }
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
    const poll = () => {
      const pads = navigator.getGamepads ? Array.from(navigator.getGamepads()).filter(Boolean) : [];
      if (pads.length && !primed) {
        // Buttons already down when the page starts (Select+Start that opened the menu) count only after a release.
        primed = true;
        const p0 = pads[0];
        [[0, 'cross'], [1, 'circle'], [9, 'start'], [12, 'up'], [13, 'down'], [14, 'left'], [15, 'right']].forEach(([i, n]) => {
          if (p0.buttons[i] && p0.buttons[i].pressed) held[n] = { next: Infinity };
        });
      }
      // While the game is starting or running the controller belongs to the game: Cross would press the launcher's Stop Game button.
      const gameBusy = !this.ingame && this.state && this.state.launch !== 'idle';
      if (pads.length && !gameBusy && !(this.state && this.state.listening)) {
        const p = pads[0];
        const btn = (i) => !!(p.buttons[i] && p.buttons[i].pressed);
        const ax = (i) => p.axes[i] || 0;
        edge('up', btn(12) || ax(1) < -0.6, () => this.navMove(-1));
        edge('down', btn(13) || ax(1) > 0.6, () => this.navMove(1));
        edge('left', btn(14) || ax(0) < -0.6, () => this.navSide(-1));
        edge('right', btn(15) || ax(0) > 0.6, () => this.navSide(1));
        edge('cross', btn(0), () => this.navPress());
        edge('circle', btn(1), () => this.navBack());
        edge('start', btn(9), () => { if (this.ingame && this.host) this.host.postMessage('resume'); });
      }
      requestAnimationFrame(poll);
    };
    requestAnimationFrame(poll);
    // In game the menu starts on Resume.
    if (this.ingame) setTimeout(() => this.navFocus(document.querySelector('.btn.go')), 150);
  }

  sig(s) {""")

L("""  sig(s) {
    return JSON.stringify({ res: s.res, renderer: s.renderer, ws: s.ws, vsync: s.vsync, tex: s.tex, fx: s.fx, aa: s.aa, msaa: s.msaa });
  }""",
  """  sig(s) {
    return JSON.stringify({ res: s.res, renderer: s.renderer, fps: s.fps, ws: s.ws, wsShape: s.wsShape, vsync: s.vsync, tex: s.tex, fx: s.fx, aa: s.aa, bind: s.bind, mods: s.mods });
  }

  send(cmd, extra) {
    if (!this.host) { this.toastMsg('Launcher host not connected'); return; }
    const s = this.state;
    const lines = [cmd];
    const add = (k, v) => lines.push(k + '=' + String(v));
    add('res', s.res); add('renderer', s.renderer); add('fps', s.fps); add('ws', s.ws ? 1 : 0); add('wsShape', s.wsShape);
    add('vsync', s.vsync ? 1 : 0); add('tex', s.tex); add('fx', s.fx); add('aa', s.aa);
    Object.keys(s.mods).forEach((k) => add('mod.' + k, s.mods[k] ? 1 : 0));
    Object.keys(s.bind).forEach((k) => add('bind.' + k, s.bind[k]));
    if (extra) Object.keys(extra).forEach((k) => add(k, extra[k]));
    this.host.postMessage(lines.join('\\n'));
  }

  onHost(msg) {
    const m = typeof msg === 'string' ? msg : '';
    if (m === 'max:1' || m === 'max:0') { document.documentElement.classList.toggle('maxed', m === 'max:1'); return; }
    if (m === 'saved') { this.setState({ saved: this.sig(this.state) }); this.toastMsg('Settings saved'); }
    else if (m === 'saved:restart') { this.setState({ saved: this.sig(this.state), restart: true }); this.toastMsg('Saved - some changes apply after a restart'); }
    else if (m === 'saved:live') { this.setState({ saved: this.sig(this.state) }); this.toastMsg('Settings applied'); }
    else if (m === 'starting') { this.setState({ launch: 'launching', progress: 10 }); this.musicOff(); }
    else if (m === 'running') { clearInterval(this.timer); this.setState({ launch: 'running', progress: 100 }); this.musicOff(); }
    else if (m === 'exited') { clearInterval(this.timer); this.setState({ launch: 'idle', progress: 0 }); this.toastMsg('Game closed'); this.musicOn(); }
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
    const RES = [['Native', '1x  640 x 448'], ['720p', '2x  1280 x 896'], ['1080p', '3x  1920 x 1344'], ['1440p', '4x  2560 x 1792'], ['4K', '6x  3840 x 2688']];""")

if guide_ids:
    for key, cap, label, _ in GUIDES:
        pass
    old_sl = re.search(r"    const SL = \[\n(.*?)\n    \];", logic, re.S).group(0)
    new_sl = "    const SL = [\n" + ",\n".join(
        "      { src: '%s', cap: %s, label: %s }" % (guide_ids[k], json.dumps(c), json.dumps(l)) for k, c, l, _ in GUIDES) + "\n    ];"
    L(old_sl, new_sl)

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
      ['full', 'Fullscreen', 'Borderless fullscreen. In game, Alt+Enter or F11 switches to a window and back.'],
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

      hasToast: !!s.toast,""")
# Game file: state, host messages, dialog, play guard.
L("""    this.state.restart = !!(init && init.restart);""",
  """    this.state.restart = !!(init && init.restart);
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
      installIso: () => { if (!this.state.iso) { this.toastMsg('Choose your disc image first (Browse...)'); return; } this.setState({ installPct: 0, installFile: '', isoMsg: '' }); if (this.host) this.host.postMessage('installiso'); },""")


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

template = template[:logic_start] + logic + template[logic_end:]
set_block('manifest', json.dumps(manifest, separators=(',', ':')))
set_block('template', json.dumps(template).replace('</', r'<\u002F'))  # keep </script> inside the block escaped
os.makedirs(os.path.dirname(out_path), exist_ok=True)
open(out_path, 'w', encoding='utf-8').write(page)
print('wrote', out_path, len(page), 'chars;', len(guide_ids), 'guide pictures')
