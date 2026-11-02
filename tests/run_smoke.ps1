param(
    [string]$Exe = ".\build\Debug\Loxis.exe"
)

$ErrorActionPreference = "Continue"

$root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$env:LOXIS_STDLIB = Join-Path $root "stdlib"
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
        Name = "function overload by type"
        File = "src\test\function_overload_type.lx"
        Out = "int:7`nstr:lox`n"
    },
    @{
        Name = "exception catch and finally"
        File = "src\test\exception_basic.lx"
        Out = "caught: boom`n42`n1`n"
    },
    @{
        Name = "exception control flow"
        File = "src\test\exception_control_flow.lx"
        Out = "finally-return`n7`nrethrow: again`nfinally-rethrow`n"
    },
    @{
        Name = "typed exception catch"
        File = "src\test\exception_typed.lx"
        Out = "specific: typed`n"
    },
    @{
        Name = "exception unwind restores state"
        File = "src\test\exception_unwind_state.lx"
        Out = "alive`nstate`n"
    },
    @{
        Name = "runtime errors are catchable"
        File = "src\test\runtime_error_catch.lx"
        Out = "caught divide: division by zero`ncaught parse: invalid integer: '12x'`ncaught heap: heap read out of bounds or through an invalid handle`n"
    },
    @{
        Name = "finally abrupt control flow"
        File = "src\test\exception_finally_control.lx"
        Out = "2`n1`n101`n102`n103`n3`n"
    },
    @{
        Name = "finally overrides pending return"
        File = "src\test\exception_finally_throws.lx"
        Out = "finally-wins`n"
    },
    @{
        Name = "array and map literals"
        File = "src\test\map_literal_success.lx"
        Out = "literal maps: 2, 2`n"
    },
    @{
        Name = "array index assignment"
        File = "src\test\array_index_assign.lx"
        Out = "index assignment ok`n"
    },
    @{
        Name = "string templates"
        File = "src\test\string_template.lx"
        Out = "sum 7`ntrue false`nhi loxis!`nline1`nline2`nliteral `$ here`ncost `$5 and lone `$ sign`n100% done`nnested smaller`nbool true`nreeval 3 3 11`n"
    },
    @{
        Name = "mutable global var"
        File = "src\test\mutable_global.lx"
        Out = "0 start`n12`ndone`n"
    },
    @{
        Name = "enum exhaustive when"
        File = "src\test\enum_exhaustive_when.lx"
        Out = "12 9 0`nsized`npoint`npoint!`n"
    },
    @{
        Name = "stdlib kotlin-style list hof"
        File = "src\test\stdlib_list_kotlin_hof.lx"
        Out = "map 1,4,9,16,25`nmapIndexed 1,12,23,34,45`nfilter 1,3,5`nfilterIndexed 1,3,5`nflatMap 1,-1,2,-2,3,-3,4,-4,5,-5`nchunked 3 1,2 5`ntakeLast 4,5`ndropLast 1,2,3`ndrop 4,5`ntakeWhile 1,2`ndropWhile 3,4,5`ndistinct 1,2,3`ndistinctBy 1,2`nindexOfFirst 1`nindexOfLast 3`nnone true`nreduce 15`nlastOrNull 5`nlastOrNull empty -1`nmirrors 1,3,5,7,9`nlistReduce 120`nlistNone true`nflatten 1,2,3,4,5`nforEachIndexed`n0:1`n1:2`n2:3`n3:4`n4:5`ndone`n"
    },
    @{
        Name = "stdlib str and list extras"
        File = "src\test\stdlib_str_list_extras.lx"
        Out = "ababab`n|`ntrue`nfalse`ntrue`na,b`ntrue`ntrue false`n"
    },
    @{
        Name = "for range"
        File = "src\test\for_range.lx"
        Out = "10`n"
    },
    @{
        Name = "for continue and labels"
        File = "src\test\for_continue_label.lx"
        Out = "continue`n0`n1`n3`n4`nbreak`n0`n1`n2`ncontinue array`n1`n2`n4`ncontinue size/get`n0`n14`n21`nnested labeled`n0`n2`n10`n12`ncontinue outer`n0`n10`n20`nempty range`ndone`n"
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
        Name = "singleton object and extension function"
        File = "src\test\object_extension.lx"
        Out = "41`n42`n42`n"
    },
    @{
        Name = "generated data class members"
        File = "src\test\data_class.lx"
        Out = "1`nAda`n2`nGrace`ntrue`ntrue`n995`nUser(id=1, name=Ada)`ntrue`n32768`nFlags(enabled=true, marker=A, ratio=1.5)`n"
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
        Out = "5`n4`n3`n12`n6`nhello-stdlib`ntrue`n"
    },
    @{
        Name = "stdlib functional"
        File = "src\test\stdlib_functional.lx"
        Out = "6`n22`n"
    },
    @{
        Name = "stdlib option result"
        File = "src\test\stdlib_option_result.lx"
        Out = "4`n7`n40`ntrue`ntrue`n11`n5`n12`ntrue`ntrue`n"
    },
    @{
        Name = "capturing closure"
        File = "src\test\capturing_closure.lx"
        Out = "15`n11`n13`n15`n10`n"
    },
    @{
        Name = "closure capture write"
        File = "src\test\closure_capture_write.lx"
        Out = "1`n2`n3`n42`n41`n"
    },
    @{
        Name = "nested closures"
        File = "src\test\closure_nested.lx"
        Out = "15`n123`n42`n"
    },
    @{
        Name = "trailing lambda"
        File = "src\test\trailing_lambda.lx"
        Out = "2`n8`n10`n42`n"
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
        Out = "true`n"
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
    $tempRoot = [System.IO.Path]::GetFullPath([System.IO.Path]::GetTempPath())
    $work = Join-Path $tempRoot ("loxis-smoke-" + [guid]::NewGuid().ToString("N"))
    [System.IO.Directory]::CreateDirectory($work) | Out-Null

    try {
        $process = New-Object System.Diagnostics.Process
        $process.StartInfo.FileName = $exePath
        $process.StartInfo.Arguments = '"' + $filePath + '"'
        $process.StartInfo.WorkingDirectory = $work
        $process.StartInfo.UseShellExecute = $false
        $process.StartInfo.CreateNoWindow = $true
        $process.StartInfo.RedirectStandardOutput = $true
        $process.StartInfo.RedirectStandardError = $true
        $process.StartInfo.RedirectStandardInput = $true
        [void]$process.Start()
        $stdoutTask = $process.StandardOutput.ReadToEndAsync()
        $stderrTask = $process.StandardError.ReadToEndAsync()
        if ($case.ContainsKey("In")) { $process.StandardInput.Write($case.In) }
        $process.StandardInput.Close()
        if (-not $process.WaitForExit(30000)) {
            $process.Kill()
            $process.WaitForExit()
            throw "Smoke test timed out: $($case.Name)"
        }
        $exitCode = $process.ExitCode
        $stdout = Normalize-Lines $stdoutTask.Result
        $stderr = Normalize-Lines $stderrTask.Result
        $expected = Normalize-Lines $case.Out
        $expectedExit = if ($case.ContainsKey("Exit")) { $case.Exit } else { 0 }
        $expectedError = if ($case.ContainsKey("Err")) { Normalize-Lines $case.Err } else { "" }

        if ($exitCode -ne $expectedExit -or $stdout -ne $expected -or $stderr -ne $expectedError) {
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
        if ($process) { $process.Dispose() }
        $resolvedWork = [System.IO.Path]::GetFullPath($work)
        if ($resolvedWork.StartsWith($tempRoot, [System.StringComparison]::OrdinalIgnoreCase) -and
            (Split-Path -Leaf $resolvedWork).StartsWith("loxis-smoke-")) {
            Remove-Item -LiteralPath $resolvedWork -Recurse -Force -ErrorAction SilentlyContinue
        }
    }
}

if ($failed -gt 0) {
    throw "$failed smoke test(s) failed"
}

Write-Host "All smoke tests passed."
