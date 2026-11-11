/**
 * Canned output of a few everyday server commands for the mock shell, so browser previews and the store / README
 * screenshots (scripts/screenshots.mjs) show a believable terminal. All names, addresses and numbers are made up.
 */

const ESC = '\x1b[';
const reset = `${ESC}0m`;
const sgr = (code: string, s: string) => `${ESC}${code}m${s}${reset}`;
const lines = (rows: string[]) => rows.join('\r\n') + '\r\n';

/** Rough htop: CPU and memory bars, load, then a process list cut to the terminal size. */
function htop(cols: number, rows: number): string {
  const width = Math.max(60, cols);
  const half = Math.floor(width / 2) - 2;
  const bar = (label: string, parts: [string, number][], text: string) => {
    const inner = half - label.length - 3;
    let s = '';
    let used = 0;
    for (const [color, frac] of parts) {
      const n = Math.round(inner * frac);
      s += sgr(color, '|'.repeat(n));
      used += n;
    }
    const room = inner - used - text.length;
    return `${sgr('36', label)}${sgr('1', '[')}${s}${' '.repeat(Math.max(0, room))}${sgr('2', text)}${sgr('1', ']')}`;
  };
  const cpu = [0.42, 0.18, 0.67, 0.25].map((f, i) => bar(` ${i + 1}`, [['32', f * 0.7], ['31', f * 0.3]], `${(f * 100).toFixed(1)}%`));
  const mem = bar('Mem', [['32', 0.38], ['34', 0.08], ['33', 0.22]], '5.41G/15.6G');
  const swp = bar('Swp', [['31', 0.02]], '96.0M/2.00G');
  const right = [
    `${sgr('36', 'Tasks:')} ${sgr('1', '142')}, ${sgr('1', '611')} thr; ${sgr('1;32', '2')} running`,
    `${sgr('36', 'Load average:')} ${sgr('1', '0.84')} 0.71 0.66`,
    `${sgr('36', 'Uptime:')} ${sgr('1', '41 days, 06:12:09')}`,
    ''
  ];
  const head: string[] = [];
  for (let i = 0; i < 6; i++) {
    const left = i < 4 ? cpu[i] : i === 4 ? mem : swp;
    head.push(`${left}  ${right[i] ?? ''}`);
  }
  const procs: [number, string, string, string, string, string, string][] = [
    [1834, 'www-data', '20', '0', '412M', '3.1', 'nginx: worker process'],
    [2211, 'deploy', '20', '0', '1.21G', '18.4', 'node /srv/app/server.js --port 3000'],
    [977, 'postgres', '20', '0', '238M', '6.2', 'postgres: 16/main: checkpointer'],
    [2290, 'deploy', '20', '0', '1.18G', '12.9', 'node /srv/app/worker.js --queue mail'],
    [612, 'redis', '20', '0', '64.2M', '1.7', 'redis-server 127.0.0.1:6379'],
    [1, 'root', '20', '0', '22.4M', '0.0', '/sbin/init'],
    [438, 'root', '20', '0', '48.1M', '0.3', '/lib/systemd/systemd-journald'],
    [889, 'root', '20', '0', '15.2M', '0.0', 'sshd: /usr/sbin/sshd -D'],
    [3012, 'deploy', '20', '0', '9.8M', '0.7', 'htop'],
    [1102, 'root', '20', '0', '1.9G', '2.4', '/usr/bin/containerd'],
    [1290, 'root', '20', '0', '2.2G', '1.1', '/usr/bin/dockerd -H fd://'],
    [745, 'syslog', '20', '0', '218M', '0.2', '/usr/sbin/rsyslogd -n -iNONE'],
    [1520, 'prometh+', '20', '0', '734M', '2.0', '/usr/local/bin/node_exporter'],
    [1601, 'root', '20', '0', '18.0M', '0.0', '/usr/sbin/cron -f']
  ];
  const header = `${ESC}30;42m${'    PID USER      PRI  NI  VIRT  CPU% Command'.padEnd(width)}${reset}`;
  const list = procs.map(([pid, user, pri, ni, virt, cpu, cmd], i) => {
    const row = `${String(pid).padStart(7)} ${user.padEnd(9)} ${pri.padStart(3)} ${ni.padStart(3)} ${virt.padStart(5)} ${cpu.padStart(5)} ${cmd}`.slice(0, width);
    return i === 1 ? `${ESC}30;46m${row.padEnd(width)}${reset}` : row.replace(cmd, sgr(user === 'root' ? '0' : '1', cmd));
  });
  let keys = '';
  let keysWidth = 0;
  for (const [i, label] of ['Help', 'Setup', 'Search', 'Filter', 'Tree', 'SortBy', 'Nice -', 'Nice +', 'Kill', 'Quit'].entries()) {
    const k = `F${i + 1}`;
    if (keysWidth + k.length + 6 > width) break; // only what fits, as htop does
    keys += `${k}${ESC}30;46m${label.padEnd(6)}${reset}`;
    keysWidth += k.length + 6;
  }
  const body = [...head, '', header, ...list];
  const fill = Math.max(0, rows - body.length - 1);
  return `${ESC}2J${ESC}H` + body.slice(0, rows - 1).join('\r\n') + '\r\n'.repeat(fill + 1) + keys;
}

const ACCESS = [
  ['203.0.113.24', 'GET /api/v1/orders?page=2 HTTP/2.0', '200', '1834'],
  ['198.51.100.7', 'GET /static/app.4f9c1e.js HTTP/2.0', '200', '48211'],
  ['203.0.113.24', 'POST /api/v1/orders HTTP/2.0', '201', '412'],
  ['192.0.2.81', 'GET /healthz HTTP/1.1', '200', '2'],
  ['198.51.100.33', 'GET /login HTTP/2.0', '200', '5120'],
  ['198.51.100.33', 'POST /api/v1/session HTTP/2.0', '401', '96'],
  ['198.51.100.33', 'POST /api/v1/session HTTP/2.0', '200', '388'],
  ['203.0.113.90', 'GET /api/v1/products/1182 HTTP/2.0', '200', '2210'],
  ['192.0.2.81', 'GET /healthz HTTP/1.1', '200', '2'],
  ['203.0.113.90', 'GET /images/hero@2x.webp HTTP/2.0', '304', '0'],
  ['198.51.100.7', 'GET /api/v1/cart HTTP/2.0', '200', '644'],
  ['203.0.113.155', 'GET /wp-login.php HTTP/1.1', '404', '162'],
  ['203.0.113.24', 'DELETE /api/v1/orders/88123 HTTP/2.0', '204', '0'],
  ['198.51.100.7', 'GET /api/v1/recommendations HTTP/2.0', '200', '3391'],
  ['192.0.2.81', 'GET /healthz HTTP/1.1', '200', '2'],
  ['203.0.113.90', 'POST /api/v1/checkout HTTP/2.0', '502', '157'],
  ['203.0.113.90', 'POST /api/v1/checkout HTTP/2.0', '200', '921']
];

function accessLog(): string {
  return lines(
    ACCESS.map(([ip, req, status, size], i) => {
      const color = status.startsWith('5') ? '1;31' : status.startsWith('4') ? '33' : '32';
      const t = `22/Oct/2026:09:${String(14 + Math.floor(i / 3)).padStart(2, '0')}:${String((i * 7) % 60).padStart(2, '0')} +0000`;
      return `${ip} - - [${t}] "${req}" ${sgr(color, status)} ${size} "-" "Mozilla/5.0"`;
    })
  );
}

const COMMANDS: Record<string, (cols: number, rows: number) => string> = {
  htop,
  'tail -f /var/log/nginx/access.log': accessLog,
  'docker ps': () =>
    lines([
      'CONTAINER ID   IMAGE                 STATUS                 PORTS                    NAMES',
      `4c1e9a07b2d3   app/api:2.14.0        ${sgr('32', 'Up 6 days (healthy)')}    127.0.0.1:3000->3000/tcp ${sgr('1', 'api')}`,
      `9f30d1c6e8a4   app/worker:2.14.0     ${sgr('32', 'Up 6 days')}              -                        ${sgr('1', 'worker')}`,
      `b7a2e5f19c0d   postgres:16-alpine    ${sgr('32', 'Up 41 days (healthy)')}   5432/tcp                 ${sgr('1', 'db')}`,
      `e18d4c3a9b72   redis:7-alpine        ${sgr('32', 'Up 41 days')}             6379/tcp                 ${sgr('1', 'cache')}`,
      `0a6f2b8d4e15   nginx:1.27-alpine     ${sgr('32', 'Up 6 days')}              0.0.0.0:443->443/tcp     ${sgr('1', 'proxy')}`
    ]),
  'df -h': () =>
    lines([
      'Filesystem      Size  Used Avail Use% Mounted on',
      '/dev/sda1        78G   31G   44G  42% /',
      'tmpfs           7.8G     0  7.8G   0% /dev/shm',
      '/dev/sda15      105M  6.1M   99M   6% /boot/efi',
      `/dev/sdb1       492G  402G   65G  ${sgr('33', '87%')} /srv/data`
    ]),
  uptime: () => ' 09:16:42 up 41 days,  6:12,  2 users,  load average: 0.84, 0.71, 0.66\r\n',
  'uname -a': () => 'Linux web-01 6.8.0-45-generic #45-Ubuntu SMP PREEMPT_DYNAMIC x86_64 GNU/Linux\r\n',
  'systemctl status nginx': () =>
    lines([
      `${sgr('1;32', '●')} nginx.service - A high performance web server and a reverse proxy server`,
      '     Loaded: loaded (/usr/lib/systemd/system/nginx.service; enabled; preset: enabled)',
      `     Active: ${sgr('1;32', 'active (running)')} since Wed 2026-10-16 03:04:11 UTC; 6 days ago`,
      '       Docs: man:nginx(8)',
      '   Main PID: 1833 (nginx)',
      '      Tasks: 5 (limit: 18921)',
      '     Memory: 14.2M (peak: 21.6M)',
      '        CPU: 3min 41.552s',
      '     CGroup: /system.slice/nginx.service',
      '             ├─1833 "nginx: master process /usr/sbin/nginx -g daemon on; master_process on;"',
      '             └─1834 "nginx: worker process"'
    ]),
  'ls -la': () =>
    lines([
      'total 48',
      `drwxr-x--- 6 deploy deploy 4096 Oct 22 09:02 ${sgr('1;34', '.')}`,
      `drwxr-xr-x 4 root   root   4096 Sep 10 11:20 ${sgr('1;34', '..')}`,
      '-rw------- 1 deploy deploy 2214 Oct 22 08:58 .bash_history',
      '-rw-r--r-- 1 deploy deploy 3771 Sep 10 11:20 .bashrc',
      `drwx------ 2 deploy deploy 4096 Sep 10 11:24 ${sgr('1;34', '.ssh')}`,
      `drwxr-xr-x 5 deploy deploy 4096 Oct 16 03:01 ${sgr('1;34', 'releases')}`,
      `lrwxrwxrwx 1 deploy deploy   24 Oct 16 03:01 ${sgr('1;36', 'current')} -> releases/2026-10-16-0301`,
      `-rwxr-xr-x 1 deploy deploy  912 Oct  2 14:47 ${sgr('1;32', 'deploy.sh')}`,
      '-rw-r--r-- 1 deploy deploy  388 Oct  2 14:47 docker-compose.yml'
    ]),
  clear: () => `${ESC}2J${ESC}H`
};

/** Output for `cmd`, or undefined when the demo shell does not know it. */
export function demoCommand(cmd: string, cols: number, rows: number): string | undefined {
  const fn = COMMANDS[cmd];
  return fn ? fn(cols, rows) : undefined;
}

export const DEMO_COMMANDS = Object.keys(COMMANDS);

/** Commands that take over the screen (no prompt after them, like a running htop). */
export function isFullScreen(cmd: string): boolean {
  return cmd === 'htop';
}

/** The login text of the demo shell (an Ubuntu server's). */
export function demoBanner(): string {
  return lines([
    'Welcome to Ubuntu 24.04.1 LTS (GNU/Linux 6.8.0-45-generic x86_64)',
    '',
    ' * Documentation:  https://help.ubuntu.com',
    '',
    '  System load:  0.84               Processes:             142',
    '  Usage of /:   42.1% of 77.35GB   Users logged in:       1',
    '  Memory usage: 35%                IPv4 address for eth0: 10.0.0.21',
    '',
    'Last login: Wed Oct 22 08:57:13 2026 from 192.0.2.10'
  ]);
}
