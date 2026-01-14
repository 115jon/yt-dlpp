$urls = @(
    @{ Name="Standard (Rick Roll)"; Url="https://www.youtube.com/watch?v=dQw4w9WgXcQ" },
    @{ Name="Oldest Video"; Url="https://www.youtube.com/watch?v=jNQXAC9IVRw" },
    @{ Name="Shorts URL Format"; Url="https://www.youtube.com/shorts/dQw4w9WgXcQ" },
    @{ Name="Embed URL Format"; Url="https://www.youtube.com/embed/dQw4w9WgXcQ" }
)

$results = @()

foreach ($item in $urls) {
    $u = $item.Url
    $name = $item.Name
    Write-Host "Testing: $name ($u)" -ForegroundColor Cyan

    # Measure yt-dlpp
    $t1 = Measure-Command {
        $p1 = Start-Process -FilePath ".\build\yt-dlpp.exe" -ArgumentList "--simulate `"$u`"" -PassThru -NoNewWindow -Wait
    }
    $status_pp = if ($p1.ExitCode -eq 0) { "PASS" } else { "FAIL" }

    # Measure yt-dlp
    $t2 = Measure-Command {
        $p2 = Start-Process -FilePath "yt-dlp" -ArgumentList "--simulate `"$u`"" -PassThru -NoNewWindow -Wait
    }
    $status_p = if ($p2.ExitCode -eq 0) { "PASS" } else { "FAIL" }

    $results += [PSCustomObject]@{
        Test = $name
        "yt-dlpp Status" = $status_pp
        "yt-dlpp Time(s)" = [math]::Round($t1.TotalSeconds, 3)
        "yt-dlp Status" = $status_p
        "yt-dlp Time(s)" = [math]::Round($t2.TotalSeconds, 3)
    }
}

$results | Format-Table -AutoSize
