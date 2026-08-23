const $ = (selector, root = document) => root.querySelector(selector);
const $$ = (selector, root = document) => [...root.querySelectorAll(selector)];

const dialog = $('#connectionDialog');
const form = $('#connectionForm');
const connectButton = $('#connectButton');
const summaryButton = $('#connectionSummary');
const stateLabel = $('#stateLabel');
const addressLabel = $('#addressLabel');
const remoteTitle = $('#remoteTitle');
const transferButton = $('#transferButton');
const transferShelf = $('#transferShelf');
const progressBar = $('#progressBar');
const progressValue = $('#progressValue');
const speedValue = $('#speedValue');
const transferName = $('#transferName');
const transferDetail = $('#transferDetail');
const footerStatus = $('#footerStatus');
const toast = $('#toast');

let connected = false;
let connectionConfig = null;
let currentTaskId = null;
let eventStream = null;
let selected = $('.file-row.selected');
let toastTimer;

function showToast(message) {
  toast.textContent = message;
  toast.classList.add('is-visible');
  clearTimeout(toastTimer);
  toastTimer = setTimeout(() => toast.classList.remove('is-visible'), 1800);
}

function setConnected(next, address = addressLabel.textContent) {
  connected = next;
  stateLabel.textContent = next ? '已配置' : '未配置';
  addressLabel.textContent = next ? address : '点击以配置服务器';
  summaryButton.classList.toggle('is-offline', !next);
  connectButton.textContent = next ? '清除' : '配置';
  footerStatus.textContent = next ? '配置就绪' : '等待配置';
}

function fileName(filePath) {
  return filePath.split(/[\\/]/).filter(Boolean).pop() || filePath;
}

function parentPath(filePath) {
  const parts = filePath.split(/[\\/]/).filter(Boolean);
  parts.pop();
  if (filePath.includes('\\')) return parts.join('\\') || '\\';
  return `/${parts.join('/')}`;
}

function formatRate(bytesPerSecond) {
  if (bytesPerSecond < 1024) return `${bytesPerSecond} B/s`;
  if (bytesPerSecond < 1024 ** 2) return `${(bytesPerSecond / 1024).toFixed(1)} KB/s`;
  return `${(bytesPerSecond / 1024 ** 2).toFixed(1)} MB/s`;
}

function openConnection() {
  if (!dialog.open) dialog.showModal();
}

connectButton.addEventListener('click', () => {
  if (connected) {
    if (eventStream) eventStream.close();
    eventStream = null;
    currentTaskId = null;
    connectionConfig = null;
    transferShelf.classList.add('is-hidden');
    setConnected(false);
    showToast('已断开连接');
  } else {
    openConnection();
  }
});

summaryButton.addEventListener('click', openConnection);

form.addEventListener('submit', (event) => {
  event.preventDefault();
  const data = new FormData(form);
  connectionConfig = {
    host: data.get('host'),
    port: Number(data.get('port')),
    username: data.get('user'),
    password: data.get('password'),
    localPath: data.get('localPath'),
    remotePath: data.get('remotePath')
  };
  const address = `${data.get('user')}@${data.get('host')} · ${parentPath(connectionConfig.remotePath)}`;
  setConnected(true, address);
  $('#localTitle').textContent = `本地 · ${parentPath(connectionConfig.localPath)}`;
  remoteTitle.textContent = `${data.get('user')}@${data.get('host')} · ${parentPath(connectionConfig.remotePath)}`;
  dialog.close();
  form.elements.password.value = '';
  showToast('SSH 连接成功');
});

dialog.addEventListener('click', (event) => {
  const box = dialog.getBoundingClientRect();
  const inside = event.clientX >= box.left && event.clientX <= box.right && event.clientY >= box.top && event.clientY <= box.bottom;
  if (!inside) dialog.close();
});

$$('.file-row').forEach((row) => {
  row.addEventListener('click', () => {
    $$('.file-row').forEach((item) => item.classList.remove('selected'));
    row.classList.add('selected');
    selected = row;
    const isRemote = row.closest('[data-side]').dataset.side === 'remote';
    transferButton.setAttribute('aria-label', isRemote ? '将所选文件下载至本地' : '将所选文件传输至服务器');
  });
});

$$('.refresh').forEach((button) => {
  button.addEventListener('click', () => {
    button.classList.remove('is-spinning');
    void button.offsetWidth;
    button.classList.add('is-spinning');
    showToast('文件列表已刷新');
  });
});

function renderTask(task) {
  const progress = Math.max(0, Math.min(100, Number(task.progress) || 0));
  progressBar.style.width = `${progress}%`;
  progressValue.textContent = `${Math.round(progress)}%`;
  speedValue.textContent = formatRate(task.bytesPerSecond || 0);

  if (task.status === 'connecting') {
    stateLabel.textContent = '连接中';
    summaryButton.classList.remove('is-offline');
    transferDetail.textContent = '正在建立 SSH 连接…';
    footerStatus.textContent = '正在连接';
  } else if (task.status === 'transferring') {
    stateLabel.textContent = '已连接';
    summaryButton.classList.remove('is-offline');
    transferDetail.textContent = task.direction === 'upload'
      ? `正在上传至 ${task.destinationPath}`
      : `正在下载至 ${task.destinationPath}`;
    footerStatus.textContent = '1 项正在传输';
  } else if (task.status === 'completed') {
    stateLabel.textContent = '已配置';
    transferDetail.textContent = `传输完成 · ${task.elapsedSeconds.toFixed(1)} 秒`;
    speedValue.textContent = '完成';
    footerStatus.textContent = '传输已完成';
    showToast(`${transferName.textContent} 传输完成`);
  } else if (task.status === 'failed') {
    stateLabel.textContent = '连接失败';
    summaryButton.classList.add('is-offline');
    transferDetail.textContent = task.error || '传输失败';
    speedValue.textContent = '失败';
    footerStatus.textContent = '传输失败';
    showToast('传输失败');
  } else if (task.status === 'cancelled') {
    stateLabel.textContent = '已配置';
    transferDetail.textContent = '传输已取消';
    speedValue.textContent = '已取消';
    footerStatus.textContent = '传输已取消';
  }

  if (['completed', 'failed', 'cancelled'].includes(task.status) && eventStream) {
    eventStream.close();
    eventStream = null;
    currentTaskId = null;
  }
}

async function startTransfer() {
  if (!connected || !connectionConfig) {
    openConnection();
    return;
  }
  if (!selected) {
    showToast('请选择一个文件');
    return;
  }

  const side = selected.closest('[data-side]').dataset.side;
  const direction = side === 'local' ? 'upload' : 'download';
  const sourcePath = direction === 'upload' ? connectionConfig.localPath : connectionConfig.remotePath;
  const destinationPath = direction === 'upload' ? connectionConfig.remotePath : connectionConfig.localPath;
  transferName.textContent = fileName(sourcePath);
  transferDetail.textContent = '正在创建传输任务…';
  transferShelf.classList.remove('is-hidden');
  footerStatus.textContent = '正在创建任务';

  try {
    const response = await fetch('/api/transfers', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({
        direction,
        host: connectionConfig.host,
        port: connectionConfig.port,
        username: connectionConfig.username,
        password: connectionConfig.password,
        sourcePath,
        destinationPath
      })
    });
    const task = await response.json();
    if (!response.ok) throw new Error(task.error || '创建传输任务失败');
    currentTaskId = task.id;
    renderTask(task);
    if (eventStream) eventStream.close();
    eventStream = new EventSource(task.eventsUrl);
    eventStream.addEventListener('progress', (event) => renderTask(JSON.parse(event.data)));
    eventStream.onerror = () => {
      if (eventStream) eventStream.close();
      eventStream = null;
    };
  } catch (error) {
    renderTask({ status: 'failed', progress: 0, bytesPerSecond: 0, error: error.message });
  }
}

transferButton.addEventListener('click', startTransfer);

$('#cancelButton').addEventListener('click', async () => {
  if (currentTaskId) {
    await fetch(`/api/transfers/${currentTaskId}`, { method: 'DELETE' }).catch(() => {});
    currentTaskId = null;
    showToast('传输已取消');
  }
  if (eventStream) eventStream.close();
  eventStream = null;
  transferShelf.classList.add('is-hidden');
  stateLabel.textContent = connected ? '已配置' : '未配置';
  summaryButton.classList.toggle('is-offline', !connected);
  footerStatus.textContent = connected ? '配置就绪' : '等待配置';
});

setConnected(false, '点击以配置服务器');
