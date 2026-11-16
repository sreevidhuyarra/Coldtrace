<#
  ColdTrace cloud side: storage, consumer group, Azure Function, settings, code.
  Safe to re-run: existing resources are kept, settings and code are updated.

    az login          # once
    ./azure/deploy.ps1                       # defaults below
    ./azure/deploy.ps1 -Hub my-hub -Suffix ab12

  Needs: Azure CLI with the azure-iot extension. Windows PowerShell 5.1 or 7.
#>
param(
    [string]$Group    = "coldtrace-rg",
    [string]$Location = "uaenorth",
    [string]$Hub      = "coldtrace-hub-2db5",
    [string]$Suffix   = "2db5"
)
$ErrorActionPreference = "Stop"
$az = (Get-Command az -ErrorAction SilentlyContinue).Source
if (-not $az) { $az = "$env:ProgramFiles\Microsoft SDKs\Azure\CLI2\wbin\az.cmd" }
function Az { & $az @args --only-show-errors; if ($LASTEXITCODE -ne 0) { throw "az $($args -join ' ') failed" } }

$storage  = "coldtracest$Suffix"          # 3-24 lowercase letters/digits, globally unique
$funcApp  = "coldtrace-fn-$Suffix"
$cg       = "coldtrace-fn"

Write-Host "== storage account $storage"
$exists = & $az storage account check-name -n $storage --query "nameAvailable" -o tsv
if ($exists -eq "true") {
    Az storage account create -n $storage -g $Group -l $Location --sku Standard_LRS `
        --kind StorageV2 --min-tls-version TLS1_2 --allow-blob-public-access false -o none
}
$storageConn = Az storage account show-connection-string -n $storage -g $Group -o tsv

Write-Host "== consumer group $cg on $Hub"
$cgs = Az iot hub consumer-group list --hub-name $Hub --query "[].name" -o tsv
if (-not ($cgs -contains $cg)) { Az iot hub consumer-group create --hub-name $Hub -n $cg -o none }

Write-Host "== function app $funcApp (Flex Consumption, Python 3.12)"
$apps = Az functionapp list -g $Group --query "[].name" -o tsv
if (-not ($apps -contains $funcApp)) {
    Az functionapp create -n $funcApp -g $Group --storage-account $storage `
        --flexconsumption-location $Location --runtime python --runtime-version 3.12 -o none
}

Write-Host "== app settings"
$ehConn = Az iot hub connection-string show -n $Hub --default-eventhub --policy-name service -o tsv
$ehName = Az iot hub show -n $Hub --query "properties.eventHubEndpoints.events.path" -o tsv
Az functionapp config appsettings set -n $funcApp -g $Group -o none --settings `
    "IOTHUB_EVENTS=$ehConn" "IOTHUB_EVENTHUB_NAME=$ehName" "IOTHUB_CONSUMER_GROUP=$cg" `
    "COLDTRACE_STORAGE=$storageConn"

Write-Host "== deploy code"
$src = Join-Path $PSScriptRoot "function_app"
$zip = Join-Path ([IO.Path]::GetTempPath()) "coldtrace-fn.zip"
if (Test-Path $zip) { Remove-Item $zip }
$files = Get-ChildItem $src -File | Where-Object { $_.Name -notlike "*.pyc" -and $_.Name -ne "local.settings.json" }
Compress-Archive -Path $files.FullName -DestinationPath $zip
Az functionapp deployment source config-zip -n $funcApp -g $Group --src $zip --build-remote true -o none
Remove-Item $zip

Write-Host "== done: $funcApp reads $Hub ($cg) and writes tables in $storage"
