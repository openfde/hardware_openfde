package vendor.openfde.display;

/**
 * 给 Java 层使用的简化接口
 * 只暴露你真正需要的方法
 */
@VintfStability
interface IOpenfdeDisplay {
	boolean openExternalDisplay();
	boolean closeExternalDisplay();
	boolean getExternalDisplayState();
}