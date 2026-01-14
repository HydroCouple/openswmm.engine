#!/bin/bash

# Define the old and new extensions
old_ext="c"  # Example: files currently ending in .txt
new_ext="cpp" # Example: new files will end in .text

# Check if old_ext is provided
if [ -z "$old_ext" ] || [ -z "$new_ext" ]; then
    echo "Usage: $0 <old_extension> <new_extension>"
    exit 1
fi

# Iterate over all files with the old extension in the current directory
for file in *."$old_ext"; do
    # Check if any files are found to prevent error message if none exist
    if [ -e "$file" ]; then
        # Construct the new filename
        # ${file%."$old_ext"} removes the old extension from the end of the filename
        new_file="${file%."$old_ext"}.$new_ext"

        # Rename the file
        mv -- "$file" "$new_file"
        echo "Renamed: $file -> $new_file"
    fi
done

echo "Renaming complete."
