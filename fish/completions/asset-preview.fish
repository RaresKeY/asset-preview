# Inspect only the command token, never filenames or option values.
function __fish_asset_preview_command
    set -l words (commandline -opc)
    set -e words[1]
    if test (count $argv) -eq 0
        test (count $words) -eq 0
        return $status
    end
    test (count $words) -gt 0; or return 1
    contains -- $words[1] $argv
end

complete -c asset-preview -n '__fish_asset_preview_command' -a 'gui open start restart stop hide list status paths licenses add material diff remove close-all select reload layout project cache settings defaults seek capture' -d 'Live asset preview'
complete -c asset-preview -n '__fish_asset_preview_command restart' -l if-running -d 'Keep a stopped service stopped'
complete -c asset-preview -n '__fish_asset_preview_command open diff' -F -d 'Asset file to open (repeatable)'
complete -c asset-preview -n '__fish_asset_preview_command remove select reload settings seek' -a '(asset-preview --complete-ids)' -f
complete -c asset-preview -n '__fish_asset_preview_command layout' -a 'single grid overview projects gallery' -f
complete -c asset-preview -n '__fish_asset_preview_command cache' -a 'stats events' -f
complete -c asset-preview -n '__fish_asset_preview_command cache' -l json -d 'Complete cache diagnostics JSON'
complete -c asset-preview -n '__fish_asset_preview_command cache' -l limit -r -d 'Recent event limit (1 to 128)'
complete -c asset-preview -n '__fish_asset_preview_command cache' -l after -r -d 'Events after sequence number'
complete -c asset-preview -n '__fish_asset_preview_command defaults' -a 'image audio video 3d file' -f
complete -c asset-preview -n '__fish_asset_preview_command layout' -l size -r -f -a 'auto 2 3 4' -d 'Grid rows/columns'
complete -c asset-preview -n '__fish_asset_preview_command add material diff' -l id -r -d 'Stable preview ID'
complete -c asset-preview -n '__fish_asset_preview_command add material diff' -l label -r -d 'Display name'
complete -c asset-preview -n '__fish_asset_preview_command add material diff' -l watch -r -F -d 'Watch source or dependency (repeatable)'
complete -c asset-preview -n '__fish_asset_preview_command add material diff' -l cwd -r -a '(__fish_complete_directories)' -d 'Generator working directory'
complete -c asset-preview -n '__fish_asset_preview_command add material' -l exec -r -d 'Explicit generator program and arguments; last option'
complete -c asset-preview -n '__fish_asset_preview_command add material diff' -l timeout -r -d 'Generator timeout in seconds'
complete -c asset-preview -n '__fish_asset_preview_command add' -l kind -r -f -a 'auto image model video audio graph file selection' -d 'Preview type'
complete -c asset-preview -n '__fish_asset_preview_command add' -l decisions -r -F -d 'Selection decisions output JSON'
complete -c asset-preview -n '__fish_asset_preview_command add' -l settings -r -d 'JSON display or animation options'
complete -c asset-preview -n '__fish_asset_preview_command material' -l normal -r -F -d 'Tangent-space normal map'
complete -c asset-preview -n '__fish_asset_preview_command material' -l orm -r -F -d 'Occlusion / roughness / metallic map'
complete -c asset-preview -n '__fish_asset_preview_command material' -l shape -r -f -a 'sphere cube plane' -d 'Material sample shape'
complete -c asset-preview -n '__fish_asset_preview_command status list' -l json -d 'Machine-readable state'
complete -c asset-preview -l help -s h -d 'Show help'

complete -c asset-preview -n '__fish_asset_preview_command' -l version -d 'Show release version'

complete -c asset-preview -n '__fish_asset_preview_command project' -a 'create current list remove' -f
complete -c asset-preview -n '__fish_asset_preview_command project' -l label -r -d 'Project display name'
complete -c asset-preview -n '__fish_asset_preview_command project' -l parent -r -a '(asset-preview --complete-projects)' -d 'Owning project for this group/gallery'
complete -c asset-preview -n '__fish_asset_preview_command project' -l json -d 'Machine-readable project list'
complete -c asset-preview -n '__fish_asset_preview_command add material diff layout' -l project -r -a '(asset-preview --complete-projects)' -d 'Stable project ID (layout also accepts all or ungrouped)'
complete -c asset-preview -n '__fish_asset_preview_command add material diff' -l project-label -r -d 'Project display name'
complete -c asset-preview -n '__fish_asset_preview_command add material diff' -l group -r -d 'Separate group/gallery within the project'
complete -c asset-preview -n '__fish_asset_preview_command add material diff' -l group-label -r -d 'Group/gallery display name'
complete -c asset-preview -n '__fish_asset_preview_command add material diff' -l no-current -d 'Keep the current project preview'
complete -c asset-preview -n '__fish_asset_preview_command add material diff' -l ungrouped -d 'Register without a project'
complete -c asset-preview -n '__fish_asset_preview_command layout' -l group-projects -d 'Group Overview by project'
complete -c asset-preview -n '__fish_asset_preview_command layout' -l no-group-projects -d 'Flat Overview'

complete -c asset-preview -n '__fish_asset_preview_command add' -l before -r -F -d 'Earlier file for live comparison'
complete -c asset-preview -n '__fish_asset_preview_command diff' -l mode -r -f -a 'inline split' -d 'Diff layout'
