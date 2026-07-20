import re
import os

def fix_vcxproj(file_path):
    with open(file_path, 'r') as f:
        content = f.read()

    pattern = re.compile(r'<ItemGroup Label="ProjectConfigurations">.*?</ItemGroup>', re.DOTALL)

    new_block = """  <ItemGroup Label="ProjectConfigurations">
    <ProjectConfiguration Include="Debug|Win32">
      <Configuration>Debug</Configuration>
      <Platform>Win32</Platform>
    </ProjectConfiguration>
    <ProjectConfiguration Include="Debug|x64">
      <Configuration>Debug</Configuration>
      <Platform>x64</Platform>
    </ProjectConfiguration>
    <ProjectConfiguration Include="Release|Win32">
      <Configuration>Release</Configuration>
      <Platform>Win32</Platform>
    </ProjectConfiguration>
    <ProjectConfiguration Include="Release|x64">
      <Configuration>Release</Configuration>
      <Platform>x64</Platform>
    </ProjectConfiguration>
    <ProjectConfiguration Include="Retail|Win32">
      <Configuration>Retail</Configuration>
      <Platform>Win32</Platform>
    </ProjectConfiguration>
    <ProjectConfiguration Include="Retail|x64">
      <Configuration>Retail</Configuration>
      <Platform>x64</Platform>
    </ProjectConfiguration>
  </ItemGroup>"""

    # Check if the pattern exists
    if pattern.search(content):
        new_content = pattern.sub(new_block, content)
        with open(file_path, 'w') as f:
            f.write(new_content)
        print(f"Fixed: {file_path}")
    else:
        print(f"Pattern not found in: {file_path}")

def main():
    base_dir = r'C:\DOOM-3-BFG'
    for root, dirs, files in os.walk(base_dir):
        for file in files:
            if file.endswith('.vcxproj'):
                fix_vcxproj(os.path.join(root, file))

if __name__ == '__main__':
    main()
