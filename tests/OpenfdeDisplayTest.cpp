#include <cstdio>
#include <cstring>

#include <vendor/openfde/display/1.2/IOpenfdeDisplay.h>

using ::android::sp;
using ::vendor::openfde::display::V1_2::IOpenfdeDisplay;

int main(int argc, char** argv) {
    sp<IOpenfdeDisplay> d = IOpenfdeDisplay::tryGetService();
    if (d == nullptr) {
        fprintf(stderr, "tryGetService(IOpenfdeDisplay@1.2) failed\n");
        return 1;
    }
    printf("got IOpenfdeDisplay@1.2\n");

    const char* cmd = argc > 1 ? argv[1] : "status";

    if (strcmp(cmd, "enable") == 0) {
        d->openExternalDisplay();
        printf("enableExternalDisplay called\n");
    } else if (strcmp(cmd, "disable") == 0) {
        d->closeExternalDisplay();
        printf("disableExternalDisplay called\n");
    } else if (strcmp(cmd, "status") == 0) {
        if(d->getExternalDisplayState()){
            printf("enabled=%d\n", 1);
        }else{    
            printf("enabled=%d\n", 0);
	}
    } else {
        fprintf(stderr, "usage: %s [enable|disable|status]\n", argv[0]);
        return 2;
    }
    return 0;
}
