import re

def parse_milkdrop_simulated(file_content):
    preset = {
        "name": "",
        "vertex_shader": "",
        "fragment_shader": "",
        "parameters": {}
    }
    current_section = ""

    lines = file_content.splitlines()
    i = 0
    while i < len(lines):
        line = lines[i].strip()
        if not line or line.startswith('#'):
            i += 1
            continue

        if line.startswith('[') and line.endswith(']'):
            current_section = line[1:-1]
            i += 1
            continue

        if current_section == "effect":
            # Handle both 'vertex_shader' and 'vertex_shader:'
            shader_key = None
            if line == "vertex_shader" or line == "vertex_shader:":
                shader_key = "vertex_shader"
            elif line == "fragment_shader" or line == "fragment_shader:":
                shader_key = "fragment_shader"

            if shader_key:
                shader_code = []
                i += 1
                while i < len(lines) and lines[i].strip() != "END":
                    shader_code.append(lines[i])
                    i += 1
                if shader_key == "vertex_shader":
                    preset["vertex_shader"] = "\n".join(shader_code)
                else:
                    preset["fragment_shader"] = "\n".join(shader_code)
                if i < len(lines) and lines[i].strip() == "END":
                    i += 1
                continue
            elif line == "shader":
                preset["vertex_shader"] = line # simplification
                preset["fragment_shader"] = line
                i += 1
                continue
        elif current_section == "preset":
            if ':' in line:
                key, value = line.split(':', 1)
                if key.strip() == "name":
                    preset["name"] = value.strip()
        elif current_section == "param":
            if ':' in line:
                key, value = line.split(':', 1)
                key = key.strip()
                value = value.strip()
                # Look for "param <index>"
                match = re.search(r'param\s+(\d+)', key)
                if match:
                    index = int(match.group(1))
                    preset["parameters"][index] = float(value)
        i += 1
    return preset

def test_parser():
    test_content = """
[preset]
name: Test Preset

[effect]
vertex_shader:
void main() {
    gl_Position = vertex.attrib[8];
}
END
fragment_shader:
void main() {
    float val = param 400;
    gl_FragColor = vec4(val, val, val, 1.0);
}
END

[param]
param 0: 1.0
param 400: 0.75
"""
    parsed = parse_milkdrop_simulated(test_content)
    print(f"Parsed: {parsed}")

    assert parsed["name"] == "Test Preset"
    assert "void main() {" in parsed["vertex_shader"]
    assert "void main() {" in parsed["fragment_shader"]
    assert parsed["parameters"][0] == 1.0
    assert parsed["parameters"][400] == 0.75
    print("All tests passed!")

if __name__ == "__main__":
    test_parser()
