import os

Import("env")

def _c_escaped(value):
    out = []
    for ch in value:
        o = ord(ch)
        if ("a" <= ch <= "z") or ("A" <= ch <= "Z") or ("0" <= ch <= "9") or ch in "._-":
            out.append(ch)
        else:
            out.append(f"\\x{o:02x}")
    return "".join(out)

def _set_define(name):
    value = os.getenv(name)
    if value:
        quoted = f'\\"{_c_escaped(value)}\\"'
        env.Append(CPPDEFINES=[(name, quoted)])

_set_define("WIFI_SSID")
_set_define("WIFI_PWD")
_set_define("DISCORD_WEBHOOK_URL")
