#!/opt/homebrew/bin/bash
set -euo pipefail

orig_dir="$(pwd)"
trap 'cd "$orig_dir"' EXIT

gitroot="$(git rev-parse --show-toplevel 2>/dev/null)" || {
    printf "%s\n" "Error: Not in a git repo." >&2
    exit 1
}

if [[ -z "$gitroot" || "$gitroot" == "/" ]]; then
    printf "%s\n" "Error: Refusing to clean; invalid git root '$gitroot'." >&2
    exit 1
fi

# Remove any dependencies.lock files anywhere in the repo
while IFS= read -r -d '' lock_file; do
    case "$lock_file" in
        "$gitroot"/*)
            printf "%s\n" "Removing: $lock_file"
            rm -f -- "$lock_file"
            ;;
        *)
            printf "%s\n" "Warning: Skipping unexpected path '$lock_file'." >&2
            ;;
    esac
done < <(find "$gitroot" -type f -name 'dependencies.lock' -print0)

# Remove any sdkconfig* files anywhere in the repo
while IFS= read -r -d '' sdkconfig_file; do
    case "$sdkconfig_file" in
        "$gitroot"/*)
            printf "%s\n" "Removing: $sdkconfig_file"
            rm -f -- "$sdkconfig_file"
            ;;
        *)
            printf "%s\n" "Warning: Skipping unexpected path '$sdkconfig_file'." >&2
            ;;
    esac
done < <(find "$gitroot" -type f -name 'sdkconfig*' -print0)

# Optional: remove generated web assets
if [[ -d "$gitroot/components/http_server/dist" ]]; then
    printf "%s\n" "Removing: $gitroot/components/http_server/dist"
    rm -rf -- "$gitroot/components/http_server/dist"
fi

# Remove any build directories under any "examples" or "components" trees
for base in "examples" "components"; do
    base_dir="$gitroot/$base"
    if [[ ! -d "$base_dir" ]]; then
        continue
    fi

    while IFS= read -r -d '' build_dir; do
        case "$build_dir" in
            "$gitroot"/*)
                printf "%s\n" "Removing: $build_dir"
                rm -rf -- "$build_dir"
                ;;
            *)
                printf "%s\n" "Warning: Skipping unexpected path '$build_dir'." >&2
                ;;
        esac
    done < <(find "$base_dir" -type d -name build -print0)
done