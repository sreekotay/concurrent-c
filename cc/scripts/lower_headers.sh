#!/bin/sh
# lower_headers.sh - Copy/lower .cch headers to .h files
#
# Usage: lower_headers.sh <input_dir> <output_dir>
#
# Transforms:
# - .cch includes to .h includes
# - T!>(E) result type syntax to CCResult_T_E (basic patterns)
# - Removes guarded CC_DECL_RESULT_SPEC blocks (will be auto-generated)

set -e

INPUT_DIR="${1:-include}"
OUTPUT_DIR="${2:-../out/include}"

echo "Lowering headers: $INPUT_DIR -> $OUTPUT_DIR"

# Find all .cch files and process them
find "$INPUT_DIR" -name '*.cch' | while read -r cch_file; do
    # Get relative path
    rel_path="${cch_file#$INPUT_DIR/}"
    
    # Build output path with .h extension
    h_path="$OUTPUT_DIR/${rel_path%.cch}.h"
    
    # Create output directory
    h_dir=$(dirname "$h_path")
    mkdir -p "$h_dir"
    
    # Only process if source is newer
    if [ ! -f "$h_path" ] || [ "$cch_file" -nt "$h_path" ]; then
        echo "  $cch_file -> $h_path"
        
        # Transform the file:
        # 1. Convert .cch includes to .h includes
        # 2. Convert basic T!>(E) patterns to CCResult_T_E
        #    (handles: int!>(CCError), bool!>(CCIoError), size_t!>(CCIoError), etc.)
        sed -e 's/\.cch"/.h"/g' \
            -e 's/\.cch>/.h>/g' \
            -e 's/int !> *(CCError)/CCResult_int_CCError/g' \
            -e 's/int!>(CCError)/CCResult_int_CCError/g' \
            -e 's/bool !> *(CCError)/CCResult_bool_CCError/g' \
            -e 's/bool!>(CCError)/CCResult_bool_CCError/g' \
            -e 's/bool !> *(CCIoError)/CCResult_bool_CCIoError/g' \
            -e 's/bool!>(CCIoError)/CCResult_bool_CCIoError/g' \
            -e 's/size_t !> *(CCError)/CCResult_size_t_CCError/g' \
            -e 's/size_t!>(CCError)/CCResult_size_t_CCError/g' \
            -e 's/size_t !> *(CCIoError)/CCResult_size_t_CCIoError/g' \
            -e 's/size_t!>(CCIoError)/CCResult_size_t_CCIoError/g' \
            -e 's/CCSlice !> *(CCIoError)/CCResult_CCSlice_CCIoError/g' \
            -e 's/CCSlice!>(CCIoError)/CCResult_CCSlice_CCIoError/g' \
            -e 's/void !> *(CCError)/CCResult_void_CCError/g' \
            -e 's/void!>(CCError)/CCResult_void_CCError/g' \
            "$cch_file" > "$h_path"
    fi
done

echo "Done."
