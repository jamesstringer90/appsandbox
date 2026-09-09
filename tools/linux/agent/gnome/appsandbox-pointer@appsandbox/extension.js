import Gio from 'gi://Gio';
import GLib from 'gi://GLib';
import Mtk from 'gi://Mtk';
import {Extension} from 'resource:///org/gnome/shell/extensions/extension.js';

export default class AppSandboxPointer extends Extension {
    enable() {
        this._uid = new Gio.Credentials().get_unix_user();
        const path = GLib.build_filenamev([
            GLib.get_user_runtime_dir(), 'appsandbox-pointer.sock',
        ]);
        const file = Gio.File.new_for_path(path);
        let info;
        try {
            info = file.query_info('unix::uid,unix::mode',
                Gio.FileQueryInfoFlags.NOFOLLOW_SYMLINKS, null);
        } catch (error) {
            if (!error.matches(Gio.IOErrorEnum, Gio.IOErrorEnum.NOT_FOUND))
                throw error;
        }
        if (info) {
            if (info.get_attribute_uint32('unix::uid') !== this._uid ||
                (info.get_attribute_uint32('unix::mode') & 0o170000) !== 0o140000)
                throw new Error('Invalid AppSandbox pointer socket');
            file.delete(null);
        }

        this._service = new Gio.SocketService({active: false});
        try {
            this._service.add_address(Gio.UnixSocketAddress.new(path),
                Gio.SocketType.STREAM, Gio.SocketProtocol.DEFAULT, null);
            this._socketFile = file;
            file.set_attribute_uint32('unix::mode', 0o600,
                Gio.FileQueryInfoFlags.NOFOLLOW_SYMLINKS, null);
            this._incoming = this._service.connect('incoming',
                (_service, connection) => this._accept(connection));
            this._service.start();
        } catch (error) {
            this.disable();
            throw error;
        }
    }

    disable() {
        if (this._service) {
            if (this._incoming)
                this._service.disconnect(this._incoming);
            this._service.stop();
            this._service.close();
            this._service = null;
            this._incoming = 0;
        }
        if (this._socketFile) {
            try {
                this._socketFile.delete(null);
            } catch (_) {
            }
            this._socketFile = null;
        }
    }

    _getPosition() {
        const unavailable = [-2147483648, -2147483648, 0, 0];
        try {
            const views = global.stage.peek_stage_views();
            if (views.length !== 1)
                return unavailable;
            const view = views[0];
            if (view.get_transform() !== Mtk.MonitorTransform.NORMAL)
                return unavailable;
            const framebuffer = view.get_onscreen();
            if (!framebuffer)
                return unavailable;
            const width = framebuffer.get_width();
            const height = framebuffer.get_height();
            const scale = view.get_scale();
            const layout = view.layout;
            const [point] = global.backend.get_cursor_tracker().get_pointer();
            if (!(width > 0 && height > 0 && Number.isFinite(scale) && scale > 0) ||
                !Number.isFinite(point.x) || !Number.isFinite(point.y))
                return unavailable;
            const x = Math.trunc((point.x - layout.x) * scale);
            const y = Math.trunc((point.y - layout.y) * scale);
            return [Math.max(0, Math.min(width - 1, x)),
                Math.max(0, Math.min(height - 1, y)), width, height];
        } catch (_) {
            return unavailable;
        }
    }

    _accept(connection) {
        try {
            if (!this._service)
                return true;
            const socket = connection.get_socket();
            const uid = socket.get_credentials().get_unix_user();
            if (uid !== 0 && uid !== this._uid)
                return true;
            const position = this._getPosition();
            const reply = new Uint8Array(16);
            const data = new DataView(reply.buffer);
            for (let i = 0; i < position.length; i++)
                data.setInt32(i * 4, position[i], true);
            socket.set_blocking(false);
            let offset = 0;
            while (offset < reply.length) {
                const written = socket.send(reply.subarray(offset), null);
                if (written <= 0)
                    break;
                offset += written;
            }
        } catch (_) {
        } finally {
            try {
                connection.close(null);
            } catch (_) {
            }
        }
        return true;
    }
}
