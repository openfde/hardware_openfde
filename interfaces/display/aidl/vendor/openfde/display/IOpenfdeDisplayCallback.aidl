package vendor.openfde.display;

/**
 * 回调：当外部屏状态变化时通知订阅方（如 SystemUI）。
 * event 取值见 IOpenfdeDisplay 文档：
 *   1 = Android 外部屏已打开
 *   2 = Android 外部屏已关闭
 *   3 = x11 端已达到支持打开外部屏的状态
 *   4 = x11 端已处于不支持打开外部屏的状态
 */
@VintfStability
interface IOpenfdeDisplayCallback {
	oneway void onExternalDisplayEvent(int event);
}
