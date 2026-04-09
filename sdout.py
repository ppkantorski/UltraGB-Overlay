##################################################################################
# File: sdout.py — SDOut Package Builder
#
# Description:
#   This script automates the creation of `sdout.zip`, a complete deployment
#   package for UltraGB Overlay. It organizes and assembles UltraGB Overlay
#   components into a proper SD card structure ready for use.
#
#   The script automatically creates the required folder structure, downloads
#   the UltraGB-Overlay repository to source theme files, and packages everything
#   into `sdout.zip` — which can be extracted directly to the root of an SD card.
#
# Related Projects:
#   - UltraGB Overlay:   https://github.com/ppkantorski/UltraGB-Overlay
#   - Ultrahand Overlay: https://github.com/ppkantorski/Ultrahand-Overlay
#   - nx-ovlloader:      https://github.com/ppkantorski/nx-ovlloader
#
#   For the latest updates or to contribute, visit the GitHub repository:
#   https://github.com/ppkantorski/UltraGB-Overlay
#
# Note:
#   This notice is part of the official project documentation and must not
#   be altered or removed.
#
# Requirements:
#   - Python 3.6+
#   - requests library (`pip install requests`)
#   - `ultragb.ovl` file in the script directory
#
# Licensed under GPLv2
# Copyright (c) 2026 ppkantorski
##################################################################################

import os
import shutil
import zipfile
import requests
from pathlib import Path
import tempfile

def download_file(url, destination):
    """Download a file from URL to destination"""
    print(f"Downloading {url}...")
    response = requests.get(url, stream=True)
    response.raise_for_status()

    with open(destination, 'wb') as f:
        for chunk in response.iter_content(chunk_size=8192):
            f.write(chunk)
    print(f"Downloaded to {destination}")

def extract_zip(zip_path, extract_to, exclude_metadata=True):
    """Extract zip file, optionally excluding metadata files"""
    print(f"Extracting {zip_path}...")
    with zipfile.ZipFile(zip_path, 'r') as zip_ref:
        for member in zip_ref.namelist():
            # Skip metadata files like ._ files and __MACOSX
            if exclude_metadata:
                if member.startswith('__MACOSX') or '._' in member:
                    continue
            zip_ref.extract(member, extract_to)
    print(f"Extracted to {extract_to}")

def create_zip_without_metadata(source_dir, output_zip):
    """Create a zip file excluding metadata files, preserving empty directories"""
    print(f"Creating {output_zip}...")
    with zipfile.ZipFile(output_zip, 'w', zipfile.ZIP_DEFLATED) as zipf:
        for root, dirs, files in os.walk(source_dir):
            # Write an explicit entry for empty directories
            if not files and not dirs:
                dir_arcname = os.path.relpath(root, source_dir) + '/'
                zipf.mkdir(dir_arcname)
                continue

            for file in files:
                # Skip metadata files
                if file.startswith('._') or file == '.DS_Store':
                    continue

                file_path = os.path.join(root, file)
                arcname = os.path.relpath(file_path, source_dir)
                zipf.write(file_path, arcname)
    print(f"Created {output_zip}")

def main():
    script_dir = Path.cwd()
    sdout_dir = script_dir / "sdout"
    sdout_zip = script_dir / "sdout.zip"
    temp_dir = tempfile.mkdtemp()

    try:
        # Clean up any existing sdout folder and zip file
        print("Cleaning up previous builds...")
        if sdout_dir.exists():
            shutil.rmtree(sdout_dir)
            print("Deleted existing sdout folder")
        if sdout_zip.exists():
            sdout_zip.unlink()
            print("Deleted existing sdout.zip")

        # Step 1: Create sdout folder structure
        # Matches the sdmc:/ layout documented in the UltraGB Overlay README.
        # roms/gb/ is intentionally excluded — users supply their own ROMs.
        print("Creating folder structure...")
        folders = [
            "switch/.overlays",
            "config/ultragb",
            "config/ultragb/ovl_themes",
            "config/ultragb/ovl_wallpapers",
            "config/ultragb/saves/internal",
            "config/ultragb/states/internal",
            "config/ultragb/settings",
        ]

        for folder in folders:
            folder_path = sdout_dir / folder
            folder_path.mkdir(parents=True, exist_ok=True)
            print(f"Created {folder_path}")

        # Step 2: Download UltraGB-Overlay repository to source theme files
        ultragb_zip = Path(temp_dir) / "ultragb-main.zip"
        ultragb_temp = Path(temp_dir) / "ultragb_temp"

        download_file(
            "https://github.com/ppkantorski/UltraGB-Overlay/archive/refs/heads/main.zip",
            ultragb_zip
        )
        extract_zip(ultragb_zip, ultragb_temp)

        # Find the extracted UltraGB folder (e.g. UltraGB-Overlay-main)
        ultragb_folders = [f for f in ultragb_temp.iterdir() if f.is_dir()]
        if not ultragb_folders:
            raise Exception("Could not find extracted UltraGB-Overlay folder")

        ultragb_root = ultragb_folders[0]

        # Step 3: Copy ovl_themes from the UltraGB repo into config/ultragb/ovl_themes
        print("Copying UltraGB ovl_themes...")
        ovl_themes_source = ultragb_root / "ovl_themes"
        ovl_themes_dest = sdout_dir / "config/ultragb/ovl_themes"

        if ovl_themes_source.exists():
            for f in ovl_themes_source.iterdir():
                if f.is_file():
                    shutil.copy2(f, ovl_themes_dest)
                    print(f"Copied {f.name}")
        else:
            print("Warning: ovl_themes folder not found in UltraGB-Overlay repository")

        # Step 4: Copy ultragb.ovl to switch/.overlays/
        print("Copying ultragb.ovl...")
        ultragb_ovl_source = script_dir / "ultragb.ovl"
        ultragb_ovl_dest = sdout_dir / "switch/.overlays"

        if ultragb_ovl_source.exists():
            shutil.copy2(ultragb_ovl_source, ultragb_ovl_dest)
            print("Copied ultragb.ovl")
        else:
            print("Warning: ultragb.ovl not found in script directory")

        # Step 5: Clean up temporary files
        print("Cleaning up temporary files...")
        shutil.rmtree(temp_dir)
        print("Temporary files deleted")

        # Step 6: Create final zip
        output_zip = script_dir / "sdout.zip"
        create_zip_without_metadata(sdout_dir, output_zip)

        print("\n✓ Successfully created sdout.zip!")
        print(f"Location: {output_zip}")

    except Exception as e:
        print(f"\n✗ Error: {e}")
        # Clean up on error
        if Path(temp_dir).exists():
            shutil.rmtree(temp_dir)
        raise

if __name__ == "__main__":
    main()