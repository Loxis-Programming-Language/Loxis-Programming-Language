param(
    [string]$Exe = ".\build\Debug\Loxis.exe"
)

$ErrorActionPreference = "Continue"

$root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$exePath = if ([System.IO.Path]::IsPathRooted($Exe)) { $Exe } else { Join-Path $root $Exe }

if (-not (Test-Path -LiteralPath $exePath)) {
    throw "Loxis executable not found: $exePath"
}

$cases = @(
    @{
        Name = "simple statements"
        File = "src\test\simple_test.lx"
        Out = "1`n2`n3`n"
    },
    @{
        Name = "local import"
        File = "src\test\import_main.lx"
        Out = "42`n"
    },
    @{
        Name = "generic function"
        File = "src\test\generics_box.lx"
        Out = "10`n"
    },
    @{
        Name = "function value"
        File = "src\test\function_value.lx"
        Out = "42`n10`n"
    },
    @{
        Name = "lambda value"
        File = "src\test\lambda_value.lx"
        Out = "6`n14`n21`n"
    },
    @{
        Name = "function overload"
        File = "src\test\function_overload.lx"
        Out = "1`n42`n"
    },
    @{
        Name = "for range"
        File = "src\test\for_range.lx"
        Out = "10`n"
    },
    @{
        Name = "stdlib core wrappers"
        File = "src\test\stdlib_core.lx"
        Out = "2`n33`n4`n130`n7`n"
    },
    @{
        Name = "class this method"
        File = "src\test\method_this.lx"
        Out = "5`n3`n"
    },
    @{
        Name = "class constructor fields"
        File = "src\test\class_ctor_fields.lx"
        Out = "10`n20`n15`n23`n"
    },
    @{
        Name = "singleton object"
        File = "src\test\singleton_object.lx"
        Out = "11`n12`n12`n"
    },
    @{
        Name = "data class generated members"
        File = "src\test\data_class.lx"
        Out = "2`n3`n1`n0`n1`n1026`nPoint(x=2, y=3)`n57`n"
    },
    @{
        Name = "stdlib generic list"
        File = "src\test\stdlib_list.lx"
        Out = "3`n4`n24`n"
    },
    @{
        Name = "stdlib list for-in"
        File = "src\test\stdlib_list_for.lx"
        Out = "15`n"
    },
    @{
        Name = "stdlib list higher-order"
        File = "src\test\stdlib_list_hof.lx"
        Out = "4`n50`n2`n70`n30`n40`n"
    },
    @{
        Name = "stdlib list fold"
        File = "src\test\stdlib_list_fold.lx"
        Out = "20`n384`n2`n1`n1`n"
    },
    @{
        Name = "stdlib list algorithms"
        File = "src\test\stdlib_list_algorithms.lx"
        Out = "1`n4`n41`n14`n8`n8`n"
    },
    @{
        Name = "stdlib math text"
        File = "src\test\stdlib_math_text.lx"
        Out = "10`n6`n81`n5`n42`nv2`n"
    },
    @{
        Name = "stdlib text ops"
        File = "src\test\stdlib_text_ops.lx"
        Out = "12`n108`nxis`n6`n1`n0`n`n"
    },
    @{
        Name = "stdlib file io"
        File = "src\test\stdlib_fileio.lx"
        Out = "1`n1`n1`n12`n6`nhello-stdlib`n1`n"
    },
    @{
        Name = "stdlib functional"
        File = "src\test\stdlib_functional.lx"
        Out = "6`n22`n"
    },
    @{
        Name = "stdlib option result"
        File = "src\test\stdlib_option_result.lx"
        Out = "4`n7`n40`n1`n1`n11`n5`n12`n1`n1`n"
    },
    @{
        Name = "capturing closure"
        File = "src\test\capturing_closure.lx"
        Out = "15`n11`n13`n15`n10`n"
    },
    @{
        Name = "enum ctor when"
        File = "src\test\enum_ctor_when.lx"
        Out = "0`n42`n"
    },
    @{
        Name = "nullable ops"
        File = "src\test\nullable_ops.lx"
        Out = "7`n42`n42`n"
    },
    @{
        Name = "module export"
        File = "src\test\module_export_main.lx"
        Out = "42`n"
    },
    @{
        Name = "re-export chain"
        File = "src\test\reexport_chain_main.lx"
        Out = "100`n200`n"
    },
    @{
        Name = "package resolve"
        File = "src\test\pkg_resolve_main.lx"
        Out = "77`n55`n"
    },
    @{
        Name = "type alias"
        File = "src\test\type_alias_check.lx"
        Out = "11`n10`n"
    },
    @{
        Name = "top-level val"
        File = "src\test\toplevel_val_check.lx"
        Out = "42`n7`n"
    },
    @{
        Name = "smart cast"
        File = "src\test\smart_cast_check.lx"
        Out = "1`n99`n"
    },
    @{
        Name = "function overloading"
        File = "src\test\overload_probe.lx"
        Out = "6`n6`n"
    },
    @{
        Name = "interface default method"
        File = "src\test\interface_default.lx"
        Out = "Hello, World`n"
    },
    @{
        Name = "interface override method"
        File = "src\test\interface_override.lx"
        Out = "custom World`n"
    },
    @{
        Name = "operator add"
        File = "src\test\operator_add.lx"
        Out = "4`n6`n"
    },
    @{
        Name = "operator unary"
        File = "src\test\operator_unary.lx"
        Out = "-7`n"
    },
    @{
        Name = "operator eq"
        File = "src\test\operator_eq.lx"
        Out = "1`n"
    },
    @{
        Name = "stdlib io"
        File = "src\test\stdlib_io.lx"
        Out = "LoxisIO`n1`na`n"
    },
    @{
        Name = "demo"
        File = "demo.lx"
        Out = "=== Loxis v2 ===`n--- Interface ---`nHello, Alvia`nHELLO, East!!!`n--- chr builtin ---`nL`no`nx`n--- Arithmetic ---`n42`n320`n--- Fibonacci ---`n0`n1`n1`n2`n3`n5`n8`n13`n21`n34`n--- Factorial ---`n120`n720`n--- Countdown ---`n3`n2`n1`n0`nDone.`n"
    }
)

function Normalize-Lines([string]$value) {
    return ($value -replace "`r`n", "`n") -replace "`r", "`n"
}

$failed = 0

foreach ($case in $cases) {
    $filePath = (Join-Path $root $case.File) -replace "\\", "/"
    $stdoutFile = [System.IO.Path]::GetTempFileName()
    $stderrFile = [System.IO.Path]::GetTempFileName()

    try {
        $cmd = '"' + $exePath + '" "' + $filePath + '" > "' + $stdoutFile + '" 2> "' + $stderrFile + '"'
        & cmd.exe /d /c $cmd
        $exitCode = $LASTEXITCODE

        $stdout = Normalize-Lines (Get-Content -LiteralPath $stdoutFile -Raw)
        $stderr = Normalize-Lines (Get-Content -LiteralPath $stderrFile -Raw)
        $expected = Normalize-Lines $case.Out

        if ($exitCode -ne 0 -or $stdout -ne $expected -or $stderr.Length -ne 0) {
            $failed += 1
            Write-Host "[FAIL] $($case.Name)"
            Write-Host "  file: $($case.File)"
            Write-Host "  exit: $exitCode"
            if ($stdout -ne $expected) {
                Write-Host "  expected stdout:"
                Write-Host $expected
                Write-Host "  actual stdout:"
                Write-Host $stdout
            }
            if ($stderr.Length -ne 0) {
                Write-Host "  stderr:"
                Write-Host $stderr
            }
        } else {
            Write-Host "[PASS] $($case.Name)"
        }
    } finally {
        Remove-Item -LiteralPath $stdoutFile -Force -ErrorAction SilentlyContinue
        Remove-Item -LiteralPath $stderrFile -Force -ErrorAction SilentlyContinue
    }
}

if ($failed -gt 0) {
    throw "$failed smoke test(s) failed"
}

Write-Host "All smoke tests passed."
