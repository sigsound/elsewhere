; Copyright (C) 2026 Fuzz LLC
; SPDX-License-Identifier: AGPL-3.0-or-later
; Inno Setup script for the Windows installer (built by .github/workflows/windows.yml).
; Build locally with:  iscc /DAppVersion=0.2.0 /DSourceDir=<folder with Elsewhere.exe> scripts\windows\Elsewhere.iss

#ifndef AppVersion
  #define AppVersion "0.0.0"
#endif
#ifndef IconFile
  ; JUCE makes this from resources/AppIcon.png when it builds Elsewhere.exe
  #define IconFile "..\..\build\Elsewhere_artefacts\JuceLibraryCode\icon.ico"
#endif
#ifndef SourceDir
  #define SourceDir "..\..\build\Elsewhere_artefacts\Release"
#endif

[Setup]
AppId={{9917342B-2840-436D-B8A4-5B56B4E48AC0}
AppName=Elsewhere
AppVersion={#AppVersion}
AppPublisher=Fuzz LLC
DefaultDirName={autopf}\Elsewhere
DefaultGroupName=Elsewhere
DisableProgramGroupPage=yes
OutputDir=..\..\dist
OutputBaseFilename=Elsewhere-{#AppVersion}-Windows-Setup
SetupIconFile={#IconFile}
UninstallDisplayIcon={app}\Elsewhere.exe
Compression=lzma2
SolidCompression=yes
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
WizardStyle=modern
PrivilegesRequiredOverridesAllowed=dialog

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"

[Files]
Source: "{#SourceDir}\Elsewhere.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#SourceDir}\ptformat.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#SourceDir}\Licenses\ptformat-source.zip"; DestDir: "{app}\Licenses"; Flags: ignoreversion

[Icons]
Name: "{group}\Elsewhere"; Filename: "{app}\Elsewhere.exe"
Name: "{autodesktop}\Elsewhere"; Filename: "{app}\Elsewhere.exe"; Tasks: desktopicon

[Run]
Filename: "{app}\Elsewhere.exe"; Description: "{cm:LaunchProgram,Elsewhere}"; Flags: nowait postinstall skipifsilent
