"""
PlatformIO pre-build script.
Converts src/web_ui.html into src/web_ui_html.h as a C byte array,
so it can be included directly without needing EMBED_TXTFILES.
"""

Import("env")
import os

html_path   = os.path.join(env.subst("$PROJECT_DIR"), "src", "web_ui.html")
header_path = os.path.join(env.subst("$PROJECT_DIR"), "src", "web_ui_html.h")

with open(html_path, "rb") as f:
    data = f.read()

with open(header_path, "w") as f:
    f.write("#pragma once\n")
    f.write("#include <stddef.h>\n\n")
    f.write("static const char web_ui_html[] = {\n    ")
    for i, b in enumerate(data):
        f.write(f"0x{b:02x},")
        if (i + 1) % 16 == 0:
            f.write("\n    ")
    f.write("0x00\n};\n\n")
    f.write(f"static const size_t web_ui_html_len = sizeof(web_ui_html) - 1;\n")

print(f"pre_build: generated web_ui_html.h ({len(data)} bytes)")
