import os
import json

def scan_assets(root_path, output_path):
    manifest = {
        "total_files": 0,
        "folders": {},
        "files": []
    }
    
    # Ensure we are targeting the actual extracted folder
    if os.path.isdir(root_path) and "FullAuto_Extracted_v2" in os.listdir(root_path):
        target_dir = os.path.join(root_path, "FullAuto_Extracted_v2")
    else:
        target_dir = root_path

    print(f"Scanning directory: {target_dir}")

    for root, dirs, files in os.walk(target_dir):
        # Determine relative path for the manifest
        try:
            rel_path = os.path.relpath(root, target_dir)
        except ValueError:
            rel_path = "."
            
        if rel_path not in manifest["folders"]:
            manifest["folders"][rel_path] = []
            
        for file in files:
            if file.lower().endswith((".xsb", ".xwb", ".pix", ".rxx", ".dds", ".wmv", ".ttf")):
                full_path = os.path.join(root, file)
                # Use relative path in the manifest for easier loading later
                manifest["files"].append({
                    "path": os.path.join(rel_path, file) if rel_path != "." else file,
                    "type": file.split('.')[-1].lower()
                })
                manifest["folders"][rel_path].append(file)
                manifest["total_files"] += 1

    with open(output_path, 'w') as f:
        json.dump(manifest, f, indent=4)
    
    print(f"Manifest created: {manifest['total_files']} files found.")

if __name__ == "__main__":
    scan_assets("/home/kmcclain/Games/X360/FullAuto-Native/assets", "/home/kmcclain/Games/X360/FullAuto-Native/assets/manifest.json")
