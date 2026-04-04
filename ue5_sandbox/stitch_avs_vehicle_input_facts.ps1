# ============================================================
#  AVS Vehicle input stitching pass for WAX
#  Collects existing Blueprint facts, optionally asks an LLM to
#  synthesize higher-level vehicle input wiring facts, and can
#  write the stitched facts back into WAX.
# ============================================================

param(
    [string]$WaxEndpoint = 'http://127.0.0.1:8080/',
    [ValidateSet('auto', 'openai', 'llama_cpp')]
    [string]$GenerationMode = 'auto',
    [string]$OpenAIBaseUrl = 'https://api.openai.com',
    [string]$OpenAIModel = 'gpt-5-mini',
    [string]$LlamaBaseUrl = 'http://127.0.0.1:8004',
    [string]$LlamaModel = 'qwen2.5-coder-7b-instruct',
    [int]$MaxRecallItems = 12,
    [int]$MaxOutputFacts = 24,
    [switch]$WriteFacts,
    [switch]$SkipGeneration,
    [switch]$NoPause
)

$ErrorActionPreference = 'Stop'

function Invoke-WaxRpc {
    param(
        [string]$Method,
        [hashtable]$Params = @{},
        [int]$Id = 1
    )

    $body = @{
        jsonrpc = '2.0'
        id      = $Id
        method  = $Method
        params  = $Params
    } | ConvertTo-Json -Depth 10 -Compress

    try {
        return Invoke-RestMethod -Uri $WaxEndpoint -Method Post -ContentType 'application/json' -Body $body
    } catch {
        throw "WAX RPC failed for method '$Method' at $WaxEndpoint. Ensure waxcpp_rag_server is running. Underlying error: $($_.Exception.Message)"
    }
}

function Get-EnvFirst {
    param([string[]]$Names)
    foreach ($name in $Names) {
        $value = [Environment]::GetEnvironmentVariable($name)
        if (-not [string]::IsNullOrWhiteSpace($value)) {
            return $value
        }
    }
    return $null
}

function Resolve-GenerationMode {
    param([string]$Mode)
    if ($Mode -ne 'auto') {
        return $Mode
    }
    $openAiKey = Get-EnvFirst @('WAXCPP_OPENAI_API_KEY', 'OPENAI_API_KEY')
    if (-not [string]::IsNullOrWhiteSpace($openAiKey)) {
        return 'openai'
    }
    return 'llama_cpp'
}

function Get-OpenAIText {
    param(
        [string]$BaseUrl,
        [string]$ApiKey,
        [string]$Model,
        [string]$SystemPrompt,
        [string]$UserPrompt
    )

    if ([string]::IsNullOrWhiteSpace($ApiKey)) {
        throw 'OpenAI API key is required for GenerationMode=openai'
    }

    $uri = $BaseUrl.TrimEnd('/') + '/v1/responses'
    $headers = @{
        Authorization = "Bearer $ApiKey"
        'Content-Type' = 'application/json'
    }
    $body = @{
        model = $Model
        instructions = $SystemPrompt
        input = $UserPrompt
        max_output_tokens = 1200
        text = @{
            format = @{ type = 'text' }
            verbosity = 'low'
        }
    } | ConvertTo-Json -Depth 8 -Compress

    $response = Invoke-RestMethod -Uri $uri -Method Post -Headers $headers -Body $body
    if (-not [string]::IsNullOrWhiteSpace($response.output_text)) {
        return [string]$response.output_text
    }
    if ($response.output) {
        foreach ($item in $response.output) {
            if ($item.content) {
                foreach ($contentItem in $item.content) {
                    if (-not [string]::IsNullOrWhiteSpace($contentItem.text)) {
                        return [string]$contentItem.text
                    }
                }
            }
        }
    }
    throw 'OpenAI response did not include output_text'
}

function Get-LlamaText {
    param(
        [string]$BaseUrl,
        [string]$ApiKey,
        [string]$Model,
        [string]$SystemPrompt,
        [string]$UserPrompt
    )

    $uri = $BaseUrl.TrimEnd('/') + '/v1/chat/completions'
    $headers = @{
        'Content-Type' = 'application/json'
    }
    if (-not [string]::IsNullOrWhiteSpace($ApiKey)) {
        $headers['Authorization'] = "Bearer $ApiKey"
    }
    $body = @{
        model = $Model
        messages = @(
            @{ role = 'system'; content = $SystemPrompt },
            @{ role = 'user'; content = $UserPrompt }
        )
        max_tokens = 1200
        temperature = 0.1
        top_p = 0.95
    } | ConvertTo-Json -Depth 8 -Compress

    $response = Invoke-RestMethod -Uri $uri -Method Post -Headers $headers -Body $body
    if ($response.choices -and $response.choices.Count -gt 0) {
        $message = $response.choices[0].message
        if ($message -and -not [string]::IsNullOrWhiteSpace($message.content)) {
            return [string]$message.content
        }
        $text = $response.choices[0].text
        if (-not [string]::IsNullOrWhiteSpace($text)) {
            return [string]$text
        }
    }
    throw 'llama-server response did not include choices[0].message.content'
}

function Extract-JsonArrayText {
    param([string]$Text)
    if ([string]::IsNullOrWhiteSpace($Text)) {
        return '[]'
    }
    $start = $Text.IndexOf('[')
    if ($start -lt 0) {
        return '[]'
    }
    $depth = 0
    for ($i = $start; $i -lt $Text.Length; $i++) {
        $ch = $Text[$i]
        if ($ch -eq '[') {
            $depth++
        } elseif ($ch -eq ']') {
            $depth--
            if ($depth -eq 0) {
                return $Text.Substring($start, $i - $start + 1)
            }
        }
    }
    return '[]'
}

function Normalize-Entity {
    param([string]$Entity)
    $trimmed = $Entity.Trim()
    if ([string]::IsNullOrWhiteSpace($trimmed)) {
        return ''
    }
    if ($trimmed.StartsWith('bp:')) {
        return $trimmed
    }
    return 'bp:' + $trimmed
}

function Is-RelevantVehicleFact {
    param(
        [string]$Entity,
        [string]$Attribute,
        [string]$Value
    )

    $haystack = ($Entity + ' ' + $Attribute + ' ' + $Value).ToLowerInvariant()
    $keywords = @(
        'avs_vehicle',
        'vehicle',
        'ia_vehicle',
        'imc_vehicle',
        'throttle',
        'brake',
        'handbrake',
        'steering',
        'clutch',
        'input',
        'driving'
    )
    foreach ($keyword in $keywords) {
        if ($haystack.Contains($keyword)) {
            return $true
        }
    }
    return $false
}

function Get-FactKey {
    param([string]$Entity, [string]$Attribute, [string]$Value)
    return ((Normalize-Entity $Entity) + '|' + $Attribute.Trim() + '|' + $Value.Trim()).ToLowerInvariant()
}

function Add-UniqueFactLines {
    param(
        [System.Collections.Generic.List[string]]$Lines,
        [System.Collections.Generic.HashSet[string]]$Seen,
        [object[]]$Facts
    )

    foreach ($fact in $Facts) {
        if ($null -eq $fact) {
            continue
        }
        $entity = [string]$fact.entity
        $attribute = [string]$fact.attribute
        $value = [string]$fact.value
        if ([string]::IsNullOrWhiteSpace($entity) -or [string]::IsNullOrWhiteSpace($attribute)) {
            continue
        }
        $key = Get-FactKey -Entity $entity -Attribute $attribute -Value $value
        if ($Seen.Add($key)) {
            $Lines.Add("{0} {1} {2}" -f (Normalize-Entity $entity), $attribute.Trim(), $value.Trim()) | Out-Null
        }
    }
}

$effectiveMode = Resolve-GenerationMode -Mode $GenerationMode

Write-Host '============================================================'
Write-Host ' AVS vehicle input stitching pass'
Write-Host " WAX:  $WaxEndpoint"
Write-Host " Mode: $effectiveMode"
Write-Host '============================================================'
Write-Host ''

$prefixes = @(
    'bp:AVS_Vehicle',
    'bp:/VehicleSystemPlugin/AVS_Vehicle',
    'bp:IA_Vehicle',
    'bp:IMC_Vehicle',
    'bp:Set Throttle',
    'bp:Set Handbrake',
    'bp:Copy Inputs'
)

$recallQueries = @(
    'AVS_Vehicle throttle brake steering handbrake clutch input action mapping context',
    'IMC_VehicleDriving IA_VehicleThrottle IA_VehicleSteering IA_VehicleLook vehicle input',
    'Set Handbrake Input Set Throttle and Brake Input RPC_Server_Steering Copy Inputs from Host AVS_Vehicle'
)

$factLines = New-Object 'System.Collections.Generic.List[string]'
$factSeen = [System.Collections.Generic.HashSet[string]]::new([System.StringComparer]::OrdinalIgnoreCase)
$existingKeys = [System.Collections.Generic.HashSet[string]]::new([System.StringComparer]::OrdinalIgnoreCase)
$recallLines = New-Object 'System.Collections.Generic.List[string]'

foreach ($prefix in $prefixes) {
    $response = Invoke-WaxRpc -Method 'fact.search' -Params @{
        entity_prefix = $prefix
        limit = 200
    }
    if ($response.facts) {
        foreach ($fact in $response.facts) {
            $existingKeys.Add((Get-FactKey -Entity ([string]$fact.entity) -Attribute ([string]$fact.attribute) -Value ([string]$fact.value))) | Out-Null
        }
        Add-UniqueFactLines -Lines $factLines -Seen $factSeen -Facts $response.facts
    }
}

foreach ($query in $recallQueries) {
    $response = Invoke-WaxRpc -Method 'recall' -Params @{ query = $query }
    if ($response.items) {
        $taken = 0
        foreach ($item in $response.items) {
            if ($taken -ge $MaxRecallItems) {
                break
            }
            $text = [string]$item.text
            if ([string]::IsNullOrWhiteSpace($text)) {
                continue
            }
            $recallLines.Add($text.Trim()) | Out-Null
            $taken++
        }
    }
}

if ($factLines.Count -eq 0 -and $recallLines.Count -eq 0) {
    throw 'No candidate AVS/input facts found in WAX.'
}

Write-Host ("Collected fact lines:   {0}" -f $factLines.Count)
Write-Host ("Collected recall lines: {0}" -f $recallLines.Count)
Write-Host ("Existing triple keys:   {0}" -f $existingKeys.Count)
Write-Host ''

$systemPrompt =
    "You are stitching previously extracted Unreal Engine 5 Blueprint facts into higher-level vehicle input wiring facts.`n" +
    "Input facts may be noisy or incomplete. Emit only high-confidence facts that are directly supported by the provided evidence.`n" +
    "Return ONLY a JSON array of objects with exactly these keys: entity, attribute, value.`n" +
    "Allowed attributes only: mapped_in_context, uses_input_action, consumes_input_action, drives_behavior, depends_on, purpose.`n" +
    "Prefer these entities when justified: AVS_Vehicle, IMC_VehicleDriving, IA_VehicleThrottle, IA_VehicleSteering, IA_VehicleLook, IA_VehicleBrake, IA_VehicleHandbrake, IA_VehicleClutch, Set Throttle and Brake Input, Set Handbrake Input, RPC_Server_Steering, Copy Inputs from Host.`n" +
    "Do not emit GUIDs, pin names, arithmetic labels, or weak guesses.`n" +
    "Do not restate low-level chunk facts unless they help connect input to behavior.`n" +
    "Emit at most $MaxOutputFacts facts."

$userPromptBuilder = New-Object System.Text.StringBuilder
[void]$userPromptBuilder.AppendLine('Task: stitch AVS vehicle input facts into higher-level relationships.')
[void]$userPromptBuilder.AppendLine()
[void]$userPromptBuilder.AppendLine('Existing facts:')
foreach ($line in ($factLines | Select-Object -First 240)) {
    [void]$userPromptBuilder.AppendLine("- $line")
}
[void]$userPromptBuilder.AppendLine()
[void]$userPromptBuilder.AppendLine('Recall snippets:')
foreach ($line in ($recallLines | Select-Object -Unique | Select-Object -First 48)) {
    [void]$userPromptBuilder.AppendLine("- $line")
}
[void]$userPromptBuilder.AppendLine()
[void]$userPromptBuilder.AppendLine('Return JSON array only.')
[void]$userPromptBuilder.AppendLine('/no_think')
$userPrompt = $userPromptBuilder.ToString()

if ($SkipGeneration) {
    Write-Host 'SkipGeneration=1, not calling any LLM.' -ForegroundColor Yellow
    Write-Host ''
    Write-Host 'Prompt preview:' -ForegroundColor Cyan
    $preview = $userPrompt
    if ($preview.Length -gt 3000) {
        $preview = $preview.Substring(0, 3000) + '...'
    }
    Write-Host $preview
    if (-not $NoPause) { pause }
    exit 0
}

$rawResponse =
    if ($effectiveMode -eq 'openai') {
        $apiKey = Get-EnvFirst @('WAXCPP_OPENAI_API_KEY', 'OPENAI_API_KEY')
        Get-OpenAIText -BaseUrl $OpenAIBaseUrl -ApiKey $apiKey -Model $OpenAIModel -SystemPrompt $systemPrompt -UserPrompt $userPrompt
    } else {
        $apiKey = Get-EnvFirst @('WAXCPP_LLAMA_GEN_API_KEY', 'WAXCPP_LLAMA_API_KEY')
        Get-LlamaText -BaseUrl $LlamaBaseUrl -ApiKey $apiKey -Model $LlamaModel -SystemPrompt $systemPrompt -UserPrompt $userPrompt
    }

$jsonArrayText = Extract-JsonArrayText -Text $rawResponse
$stitchedFacts = @()
if (-not [string]::IsNullOrWhiteSpace($jsonArrayText)) {
    $parsed = $jsonArrayText | ConvertFrom-Json
    if ($parsed -is [System.Collections.IEnumerable]) {
        $stitchedFacts = @($parsed)
    }
}

$allowedAttributes = [System.Collections.Generic.HashSet[string]]::new([System.StringComparer]::OrdinalIgnoreCase)
foreach ($attribute in @('mapped_in_context', 'uses_input_action', 'consumes_input_action', 'drives_behavior', 'depends_on', 'purpose')) {
    $allowedAttributes.Add($attribute) | Out-Null
}

$candidateFacts = New-Object 'System.Collections.Generic.List[object]'
$candidateSeen = [System.Collections.Generic.HashSet[string]]::new([System.StringComparer]::OrdinalIgnoreCase)

foreach ($fact in $stitchedFacts) {
    if ($null -eq $fact) {
        continue
    }
    $entity = Normalize-Entity ([string]$fact.entity)
    $attribute = ([string]$fact.attribute).Trim()
    $value = ([string]$fact.value).Trim()
    if ([string]::IsNullOrWhiteSpace($entity) -or [string]::IsNullOrWhiteSpace($attribute) -or [string]::IsNullOrWhiteSpace($value)) {
        continue
    }
    if (-not $allowedAttributes.Contains($attribute)) {
        continue
    }
    if (-not (Is-RelevantVehicleFact -Entity $entity -Attribute $attribute -Value $value)) {
        continue
    }
    $key = Get-FactKey -Entity $entity -Attribute $attribute -Value $value
    if ($existingKeys.Contains($key)) {
        continue
    }
    if (-not $candidateSeen.Add($key)) {
        continue
    }
    $candidateFacts.Add([pscustomobject]@{
        entity = $entity
        attribute = $attribute
        value = $value
    }) | Out-Null
}

Write-Host ("LLM stitched candidates: {0}" -f $candidateFacts.Count)
Write-Host ''

foreach ($fact in $candidateFacts) {
    Write-Host ("[CANDIDATE] {0} {1} {2}" -f $fact.entity, $fact.attribute, $fact.value) -ForegroundColor Cyan
}

if (-not $WriteFacts) {
    Write-Host ''
    Write-Host 'Preview only. Re-run with -WriteFacts to persist stitched facts into WAX.' -ForegroundColor Yellow
    if (-not $NoPause) { pause }
    exit 0
}

$modelLabel =
    if ($effectiveMode -eq 'openai') {
        $OpenAIModel
    } else {
        $LlamaModel
    }

$written = 0
foreach ($fact in $candidateFacts) {
    $response = Invoke-WaxRpc -Method 'fact.add' -Params @{
        entity = $fact.entity
        attribute = $fact.attribute
        value = $fact.value
        metadata = @{
            enricher_kind = 'stitch_llm'
            stitch_domain = 'avs_vehicle_input'
            stitch_model = $modelLabel
        }
    }
    if ($response.id) {
        $written++
        Write-Host ("[ADDED] #{0} {1} {2} {3}" -f $response.id, $fact.entity, $fact.attribute, $fact.value) -ForegroundColor Green
    }
}

Write-Host ''
Write-Host ("Persisted stitched facts: {0}" -f $written) -ForegroundColor Green
if (-not $NoPause) { pause }
exit 0
