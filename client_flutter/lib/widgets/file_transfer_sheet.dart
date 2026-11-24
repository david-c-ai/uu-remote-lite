import 'package:flutter/material.dart';

class FileTransferSheet extends StatefulWidget {
  const FileTransferSheet({
    super.key,
    required this.onSendFilePath,
    required this.onSyncClipboardText,
  });

  final bool Function(String path) onSendFilePath;
  final void Function(String text) onSyncClipboardText;

  @override
  State<FileTransferSheet> createState() => _FileTransferSheetState();
}

class _FileTransferSheetState extends State<FileTransferSheet> {
  final _fileCtrl = TextEditingController();
  final _clipCtrl = TextEditingController();
  String _status = '';

  @override
  void dispose() {
    _fileCtrl.dispose();
    _clipCtrl.dispose();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    return Padding(
      padding: const EdgeInsets.all(20),
      child: Column(
        mainAxisSize: MainAxisSize.min,
        crossAxisAlignment: CrossAxisAlignment.stretch,
        children: [
          const Text(
            'P2P 分片断点续传 & 剪贴板同步 (P2 可靠通道 - 不抢占画面带宽)',
            style: TextStyle(fontSize: 15, fontWeight: FontWeight.w600),
          ),
          const SizedBox(height: 16),
          Row(
            children: [
              Expanded(
                child: TextField(
                  controller: _fileCtrl,
                  decoration: const InputDecoration(
                    labelText: '本地发送文件路径',
                    hintText: '/path/to/archive.zip',
                    border: OutlineInputBorder(),
                    isDense: true,
                  ),
                ),
              ),
              const SizedBox(width: 12),
              FilledButton.icon(
                onPressed: () {
                  final path = _fileCtrl.text.trim();
                  if (path.isEmpty) return;
                  final ok = widget.onSendFilePath(path);
                  setState(() {
                    _status = ok ? '已加入 P2 分片传输队列 (含 SHA-256 校验)' : '文件读取失败，请检查路径';
                  });
                },
                icon: const Icon(Icons.upload_file_rounded, size: 18),
                label: const Text('极速发送'),
              ),
            ],
          ),
          const SizedBox(height: 14),
          Row(
            children: [
              Expanded(
                child: TextField(
                  controller: _clipCtrl,
                  decoration: const InputDecoration(
                    labelText: '跨端同步剪贴板文本 (LZ4 压缩)',
                    border: OutlineInputBorder(),
                    isDense: true,
                  ),
                ),
              ),
              const SizedBox(width: 12),
              OutlinedButton.icon(
                onPressed: () {
                  final text = _clipCtrl.text;
                  if (text.isEmpty) return;
                  widget.onSyncClipboardText(text);
                  setState(() => _status = '剪贴板已压缩推送至被控端');
                },
                icon: const Icon(Icons.content_paste_go_rounded, size: 18),
                label: const Text('同步剪贴板'),
              ),
            ],
          ),
          if (_status.isNotEmpty) ...[
            const SizedBox(height: 12),
            Text(_status, style: const TextStyle(color: Colors.tealAccent, fontSize: 12)),
          ],
        ],
      ),
    );
  }
}
