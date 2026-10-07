"""Title isolation and executable fingerprint checks for Windows launch preparation."""
import hashlib
import json
from pathlib import Path
from prepare import sfo

BLOODBORNE_IDS = frozenset(('CUSA00207', 'CUSA00208', 'CUSA00900', 'CUSA01363', 'CUSA03173', 'CUSA03023'))
SECOND_SON_SHA256 = '2d1ca79630d7bbe6fa29575f59aa7996d43071d74ac8e4f7d40137967d9039ab'
SECOND_SON_LOCALE = dict(region='HK',system_language=10,timezone_minutes=480,confirm_button='circle')

def console_locale(data):
    path=Path(data)/'infamous-locale.json'
    locale=dict(SECOND_SON_LOCALE)
    if path.is_file(): locale.update(json.loads(path.read_text(encoding='utf-8')))
    return validate_console_locale(locale,path)

def validate_console_locale(locale,source='console locale'):
    if (locale['region']!='HK' or type(locale['system_language']) is not int or
        not 0<=locale['system_language']<=29 or type(locale['timezone_minutes']) is not int or
        not -720<=locale['timezone_minutes']<=840 or locale['confirm_button'] not in ('circle','cross') or
        type(locale.get('show_fps',True)) is not bool):
        raise ValueError(f'Invalid Second Son console locale: {source}')
    return locale


def select_profile(game):
    game = Path(game)
    metadata = sfo((game / 'sce_sys/param.sfo').read_bytes())
    title = metadata.get('TITLE_ID')
    if title in BLOODBORNE_IDS:
        return dict(id='bloodborne', title_id=title, title=metadata.get('TITLE', 'Bloodborne'),
                    resource_root='dvdroot_ps4', address_patches=True)
    if title != 'CUSA00309':
        raise ValueError(f'Unsupported title {title!r}; no game-specific patches were applied.')
    with (game / 'eboot.bin').open('rb') as stream:
        digest = hashlib.file_digest(stream, 'sha256').hexdigest()
    if digest != SECOND_SON_SHA256:
        raise ValueError(f'CUSA00309 executable is not the audited build: {digest}. '
                         'Audit this version before launching; no address patches were applied.')
    if not (game / 'art').is_dir():
        raise ValueError('Second Son requires the art resource directory.')
    return dict(id='infamous', title_id=title, title=metadata.get('TITLE', 'inFAMOUS Second Son'),
                resource_root='art', address_patches=False, source_sha256=digest)


def native_environment(profile, data, env):
    """Force the safe baseline even when old Bloodborne environment settings are inherited."""
    if profile['id'] != 'infamous':
        return
    for key in ('BB_RENDER_RES', 'BB_OUTPUT_RES', 'BB_AUTO_RENDER_RES', 'BB_UI_TRIGGER_VS',
                'BB_UPSCALE_BEFORE_CS', 'BB_PATCHES', 'BB_PRESET_FILE', 'BB_TOGGLE_FILE', 'BB_DMEM_MB'):
        env.pop(key, None)
    state = Path(data) / 'profiles' / profile['title_id']
    source=Path(env.get('BB_GAME_DIR',Path(data)/'patches'/profile['title_id']))/'art/cache/all_shaders.xpps'
    locale=console_locale(data)
    # Mailbox and FIFO synchronize presentation; disallow tearing modes for this title.
    if env.get('BB_PRESENT_MODE') not in ('Mailbox','Fifo'):
        env['BB_PRESENT_MODE']='Mailbox'
    env.setdefault('BB_FLAT_DATA_MEMO','1')
    env.update(BB_GAME_PROFILE='infamous', BB_UPSCALER='none', BB_DEBUG_MOTION='0',
               BB_REGION=locale['region'], BB_LANGUAGE=str(locale['system_language']),
               BB_TIMEZONE_MINUTES=str(locale['timezone_minutes']),
               BB_ENTER_BUTTON='0' if locale['confirm_button']=='circle' else '1',
               BB_DRAW_PIPE='0',
               BB_PREP_PRIORITY='low',
               BB_SHADER_SOURCE=str(source),
               BB_LIVE_RES='0', BB_FPS='30', BB_VBLANK_HZ='60',
               BB_SHOW_FPS='1' if locale.get('show_fps',True) else '0',
               BB_USER_DIR=str(state / 'user'), BB_GPU_USER_DIR=str(state / 'gpu'),
               BB_CONFIG=str(state / 'settings.ini'), BB_USER_NAME='Delsin')
