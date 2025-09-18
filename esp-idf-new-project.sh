#!/bin/bash

# --- Function to prompt for user input and handle empty input ---
get_input() {
    local prompt_message="$1"
    local variable_name="$2"
    local default_value="$3"
    
    # Check if a default value is provided and if the user input is empty
    if [ -n "$default_value" ]; then
        echo -n "$prompt_message [$default_value]: "
        read user_input
        if [ -z "$user_input" ]; then
            eval "$variable_name=\"$default_value\""
        else
            eval "$variable_name=\"$user_input\""
        fi
    else
        echo -n "$prompt_message: "
        read user_input
        eval "$variable_name=\"$user_input\""
    fi
}

# --- Prompt for Project Name ---
get_input "Enter the name of your new ESP-IDF project" PROJECT_NAME "new_project"
echo "Creating project: $PROJECT_NAME"

# --- Prompt for target chip ---
echo
get_input "Choose the target chip (esp32 or esp32-c3)" CHIP "esp32"
echo "Targeting chip: $CHIP"

# --- Prompt for language extension ---
echo
get_input "Choose the language extension for main file (.c or .cpp)" FILE_EXT "c"
MAIN_FILE="main.$FILE_EXT"

# --- Prompt for additional directories ---
echo
echo "Do you want to create additional component directories? (e.g., 'components', 'drivers', etc.)"
echo "Enter a space-separated list of names, or leave blank for none."
read -p "Additional directories: " ADD_DIRS

# --- Prompt for components ---
echo
echo "Enter a space-separated list of components to add (e.g., 'wifi bluetooth nvs_flash')."
echo "Leave blank for none."
read -p "Components: " COMPONENTS

# --- Create the project directory structure ---
mkdir -p "$PROJECT_NAME/main"

# Create additional directories if they were specified
for dir in $ADD_DIRS; do
    mkdir -p "$PROJECT_NAME/$dir"
done

# --- Create the top-level CMakeLists.txt file ---
cat << EOF > "$PROJECT_NAME/CMakeLists.txt"
cmake_minimum_required(VERSION 3.16)
include(\$ENV{IDF_PATH}/tools/cmake/project.cmake)
set(IDF_TARGET $CHIP)
project($PROJECT_NAME)
EOF

echo "Created top-level CMakeLists.txt"

# --- Build the REQUIRES list for the main component ---
REQUIRES_LIST=""
for component in $COMPONENTS; do
    case "$component" in
        wifi)
            REQUIRES_LIST+="esp_wifi "
            ;;
        bluetooth|ble)
            REQUIRES_LIST+="esp_bt "
            ;;
        ethernet|eth)
            REQUIRES_LIST+="ethernet "
            ;;
        nvs|nvs_flash)
            REQUIRES_LIST+="nvs_flash "
            ;;
        *)
            # Add other components directly
            REQUIRES_LIST+="$component "
            ;;
    esac
done

# --- Create the main component's CMakeLists.txt file ---
cat << EOF > "$PROJECT_NAME/main/CMakeLists.txt"
idf_component_register(SRCS "$MAIN_FILE"
                       INCLUDE_DIRS "."
                       REQUIRES $REQUIRES_LIST)
EOF

echo "Created main component's CMakeLists.txt"

# --- Create the main source file with app_main function ---
if [ "$FILE_EXT" == "c" ]; then
    cat << 'EOF' > "$PROJECT_NAME/main/$MAIN_FILE"
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

void app_main(void) {
    printf("Hello from main.c!\n");
    // Start your application code here
}
EOF
elif [ "$FILE_EXT" == "cpp" ]; then
    cat << 'EOF' > "$PROJECT_NAME/main/$MAIN_FILE"
#include <cstdio>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

extern "C" {
    void app_main(void) {
        printf("Hello from main.cpp!\n");
        // Start your application code here
    }
}
EOF
else
    # For other extensions, create an empty file and inform the user
    touch "$PROJECT_NAME/main/$MAIN_FILE"
    echo "Warning: No boilerplate code was added for the '$FILE_EXT' extension."
fi

echo "Created $MAIN_FILE with boilerplate code."
echo
echo "Project '$PROJECT_NAME' created successfully in the current directory."
echo "You can now navigate into it and start building: cd $PROJECT_NAME"
echo "Run 'idf.py build' to begin."

