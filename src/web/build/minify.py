import shutil
from pathlib import Path
import subprocess
Import("env")

SRC = Path("src/web")
DST = Path("src/web/build")

DST.mkdir(exist_ok=True)


def command_exists(cmd):
    """Check if a command is available."""
    try:
        subprocess.run(
            cmd, shell=True, capture_output=True, check=False, timeout=1
        )
        return True
    except:
        return False


def run(cmd):
    subprocess.check_call(cmd, shell=True)


# Check which minifiers are available
has_html_minifier = command_exists("html-minifier-terser --version")
has_cleancss = command_exists("cleancss --version")
has_terser = command_exists("terser --version")

if not (has_html_minifier and has_cleancss and has_terser):
    print("Warning: Minifiers not found. Skipping minification.")
    print("Using existing pre-generated files from repository.")
    if not has_html_minifier:
        print("  - html-minifier-terser: not installed")
    if not has_cleancss:
        print("  - clean-css-cli: not installed")
    if not has_terser:
        print("  - terser: not installed")
    print("To enable minification, install: npm install -g html-minifier-terser clean-css-cli terser")
    exit(0)

for f in SRC.iterdir():
    if not f.is_file():
        continue

    out = DST / f.name

    if f.suffix == ".html":
        run(
            f'html-minifier-terser "{f}" '
            "--collapse-whitespace "
            "--remove-comments "
            "--minify-css true "
            "--minify-js true "
            f'-o "{out}"'
        )

    elif f.suffix == ".css":
        run(f'cleancss -O2 "{f}" -o "{out}"')

    elif f.suffix == ".js":
        run(f'terser "{f}" -c -m -o "{out}"')

    else:
        shutil.copy(f, out)

print("Web assets minified")
