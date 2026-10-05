function asset-preview --description 'Open or connect to the live asset preview window'
    set -l project "$HOME/workspace/asset-preview"
    if set -q ASSET_PREVIEW_PROJECT
        set project "$ASSET_PREVIEW_PROJECT"
    end
    command "$project/bin/asset-preview" $argv
end
