param(
    [string]$Compiler = "C:\Program Files\LLVM\bin\clang.exe"
)

$ErrorActionPreference = "Stop"

if (-not (Test-Path $Compiler)) {
    throw "clang.exe를 찾을 수 없습니다: $Compiler"
}

$common = @(
    "-D_CRT_SECURE_NO_WARNINGS",
    "-std=c11",
    "-Wall",
    "-Wextra",
    "-pedantic",
    "-Iinclude"
)

$shared = @(
    "src\engine.c",
    "src\util.c",
    "src\ast.c",
    "src\tokenizer.c",
    "src\parser.c",
    "src\optimizer.c",
    "src\storage.c",
    "src\executor.c",
    "src\database.c",
    "src\bptree.c"
)

function Invoke-Clang {
    param(
        [string]$Output,
        [string[]]$ArgsList
    )

    & $Compiler @ArgsList
    if ($LASTEXITCODE -ne 0) {
        throw "Build failed for $Output"
    }
}

Invoke-Clang "mini_sql.exe" ($common + @("src\main.c") + $shared + @("-o", "mini_sql.exe"))
Invoke-Clang "mini_sql_tests.exe" ($common + @("src\test_main.c") + $shared + @("-o", "mini_sql_tests.exe"))
Invoke-Clang "mini_sql_server.exe" ($common + @("src\server_main.c", "src\api_server.c", "src\thread_pool.c", "src\db_api.c") + $shared + @("-lws2_32", "-o", "mini_sql_server.exe"))
Invoke-Clang "mini_sql_api_tests.exe" ($common + @("tests\api_tests.c", "src\thread_pool.c", "src\db_api.c") + $shared + @("-lws2_32", "-o", "mini_sql_api_tests.exe"))
Invoke-Clang "mini_sql_benchmark.exe" ($common + @("src\benchmark_main.c") + $shared + @("-o", "mini_sql_benchmark.exe"))
Invoke-Clang "mini_sql_seed.exe" ($common + @("src\seed_main.c") + $shared + @("-o", "mini_sql_seed.exe"))

Write-Host "Built mini_sql.exe, mini_sql_tests.exe, mini_sql_server.exe, mini_sql_api_tests.exe, mini_sql_benchmark.exe, mini_sql_seed.exe"
