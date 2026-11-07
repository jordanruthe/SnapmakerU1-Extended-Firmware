# Filament Manager UI tests

`test_library.py` is a Playwright test of the filament library dialogs. It serves
`root/usr/local/filament-ui/html/` with a mocked Moonraker (WebSocket and HTTP),
so no printer is needed.

    pip install --user playwright && python3 -m playwright install chromium
    python3 test_library.py [html_dir] [screenshot_dir]

Screenshots go to a temporary directory (printed at start) unless given.
