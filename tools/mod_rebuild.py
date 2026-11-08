"""Quick rebuild for mod work: recompile changed runtime/mod sources and relink.

Unlike update.cmd this does not regenerate the 24k translated game functions,
so only edited files recompile. It rebuilds every Ready imported language
package (e.g. English), which is the program you actually play.
"""
from pathlib import Path
import configparser
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
from common import ROOT, run, visual_studio


def main():
    cmake, ninja, environment = visual_studio()
    built = 0
    for registration in sorted((ROOT/'runtime/languages').glob('*.ini')):
        language = configparser.ConfigParser()
        language.read(registration, encoding='utf-16')
        if language.get('Language', 'State', fallback='') != 'Ready':
            continue
        package = ROOT/'runtime/languages/packages'/language['Language']['Package']
        if not (package/'build/CMakeCache.txt').is_file():
            continue
        header = package/'generated/recomp_types.h'
        source = ROOT/'config/recomp_types.h'
        if header.is_file() and header.read_bytes() != source.read_bytes():
            print('Updating', header.relative_to(ROOT), 'from config/recomp_types.h (all game code recompiles, ~1 hour total).', flush=True)
            header.write_bytes(source.read_bytes())
        print('Rebuilding language package', package.name, '...', flush=True)
        print('Linking can stay on "Linking C executable" for 10-30 minutes; that is normal.', flush=True)
        run([cmake, '--build', package/'build', '--target', 'smt9_recomp', '--parallel', '4'], env=environment)
        built += 1
    if not built:
        raise SystemExit('No imported language package found to rebuild.')
    print('Mod rebuild complete. Start the game as usual.', flush=True)


if __name__ == '__main__':
    main()
