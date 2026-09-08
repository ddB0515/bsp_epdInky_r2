#!/usr/bin/env bash
#
# Activate the ESP-IDF environment for this project.
#
# Usage (note the leading dot - it must be sourced, not executed):
#
#     . ./export-idf.sh
#
# Why this exists instead of the usual $IDF_PATH/export.sh:
#
#   This machine has an EIM (ESP-IDF Installation Manager) style install, where
#   the Python virtual environments live in
#   .espressif/tools/python/<version>/venv rather than the classic
#   .espressif/python_env/idf<ver>_py<ver>_env. The stock export.sh looks for
#   the classic layout and aborts with "Python virtual environment not found".
#   EIM installs ship their own activate script instead, which this wraps.
#
#   That activate script also does not put $IDF_PATH/tools on PATH, so idf.py
#   stays invisible; the export below fixes that.

IDF_VERSION="${IDF_VERSION:-v6.0.2}"
ACTIVATE="$HOME/.espressif/tools/activate_idf_${IDF_VERSION}.sh"

if [ ! -f "$ACTIVATE" ]; then
    echo "ESP-IDF ${IDF_VERSION} activation script not found at:" >&2
    echo "  $ACTIVATE" >&2
    echo >&2
    echo "Installed versions:" >&2
    ls "$HOME/.espressif/tools/" 2>/dev/null | sed -n 's/^activate_idf_\(.*\)\.sh$/  \1/p' >&2
    return 1 2>/dev/null || exit 1
fi

# shellcheck source=/dev/null
. "$ACTIVATE"

# The EIM activate script sets IDF_PATH but leaves idf.py off PATH.
case ":$PATH:" in
    *":$IDF_PATH/tools:"*) ;;
    *) export PATH="$IDF_PATH/tools:$PATH" ;;
esac

echo "ESP-IDF ready: $(idf.py --version 2>/dev/null)"
echo "IDF_PATH: $IDF_PATH"
