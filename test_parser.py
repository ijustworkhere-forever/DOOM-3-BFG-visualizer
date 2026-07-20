import re

def parse_milkdrop(file_path):
    preset = {
        "name": "",
        "vertex_shader": "",
        "fragment_shader": "",
        "parameters": {}
    }
    current_section = ""

    with open(file_path, 'r') as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith('#'):
                continue

            if line.startswith('[') and line.endswith(']'):
                current_section = line[1:-1]
                continue

            if ':' in line:
                key, value = line.split(':', 1)
                key = key.strip()
                value = value.strip()

                if current_section == "preset":
                    if key == "name":
                        preset["name"] = value
                elif current_section == "effect":
                    if key in ["vertex_shader", "fragment_shader", "shader"]:
                        preset["vertex_shader"] = value
                        preset["fragment_shader"] = value
                elif current_section == "param":
                    try:
                        preset["parameters"][key] = float(value)
                    except ValueError:
                        pass

    return preset

if __name__ == "__main__":
    import sys
    res = parse_milkdrop(sys.argv[1])
    print(res)
