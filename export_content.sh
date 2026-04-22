#!/bin/bash

OUTPUT_FILE="esp_project_context.txt"

echo "Exportálás indul: $OUTPUT_FILE"

# 1. Projekt struktúra rögzítése
echo "================================================================================" > "$OUTPUT_FILE"
echo "PROJECT STRUCTURE:" >> "$OUTPUT_FILE"
echo "================================================================================" >> "$OUTPUT_FILE"

if command -v tree &> /dev/null; then
    tree -I "build|bld|.git|managed_components|.qtcreator" >> "$OUTPUT_FILE"
else
    find . -type d \( -name "build" -o -name "bld" -o -name ".qtcreator" -o -name ".git" -o -name "managed_components" \) -prune -o -print >> "$OUTPUT_FILE"
fi

echo -e "\n================================================================================" >> "$OUTPUT_FILE"
echo "FILE CONTENTS:" >> "$OUTPUT_FILE"
echo "================================================================================" >> "$OUTPUT_FILE"

# 2. Releváns fájlok tartalmának kigyűjtése
find . -type f \
    \( -name "*.c" -o -name "*.cpp" -o -name "*.h" -o -name "*.hpp" \
       -o -name "CMakeLists.txt" -o -name "sdkconfig.defaults" \
       -o -name "*.html" -o -name "*.css" -o -name "*.js" -o -name "*.json" \) \
    -not -path "*/build/*" \
    -not -path "*/bld/*" \
    -not -path "*/.git/*" \
    -not -path "*/managed_components/*" | sort | while read -r file; do
    
    # Csak szöveges fájlok feldolgozása (binárisok, pl. képek kizárása a web mappából)
    if file "$file" | grep -q "text"; then
        echo -e "\n--- FILE: $file ---" >> "$OUTPUT_FILE"
        cat "$file" >> "$OUTPUT_FILE"
        echo -e "\n--- END OF FILE ---" >> "$OUTPUT_FILE"
    fi
done

echo "Exportálás kész. A kimenet a(z) $OUTPUT_FILE fájlban található."
