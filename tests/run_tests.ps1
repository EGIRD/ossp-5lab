New-Item -ItemType Directory -Force -Path "sandbox", "data", "diagrams" | Out-Null
1..10 | ForEach-Object { "Small file line $_" } | Set-Content "sandbox/small.txt"
1..500 | ForEach-Object { "Large file line $_ with payload data to test TCP framing" } | Set-Content "sandbox/large.txt"
"Secret outside sandbox" | Set-Content "secret_outside.txt"

$port = 20094

Remove-Item -ErrorAction SilentlyContinue "data/exp_1client.csv", "data/exp_3clients.csv"

Write-Host "--- Experiment 1: 1 client (small file, 100 requests, 3 runs) ---"
1..3 | ForEach-Object {
    .\client.exe --host 127.0.0.1 --port $port --id 1 --count 100 --cmd "READ_FIRST small.txt 5" --csv "data/exp_1client.csv"
}

Write-Host "--- Experiment 2: 3 concurrent clients (large file, 100 requests each, 3 runs) ---"
1..3 | ForEach-Object {
    $jobs = 1..3 | ForEach-Object {
        $cid = $_
        Start-Job -ScriptBlock {
            param($p, $id, $dir)
            Set-Location $dir
            .\client.exe --host 127.0.0.1 --port $p --id $id --count 100 --cmd "READ_FIRST large.txt 50" --csv "data/exp_3clients.csv"
        } -ArgumentList $port, $cid, $PWD.Path
    }
    $jobs | Wait-Job | Receive-Job
    $jobs | Remove-Job
}