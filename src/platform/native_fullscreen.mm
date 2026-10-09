#include "platform/native_fullscreen.hpp"

#import <AppKit/AppKit.h>
#define GLFW_EXPOSE_NATIVE_COCOA
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>

#include <unordered_set>

namespace osc::platform {

namespace {

std::unordered_set<void*>& observed() {
    static std::unordered_set<void*> windows;
    return windows;
}

std::unordered_set<void*>& moving() {
    static std::unordered_set<void*> windows;
    return windows;
}

void observe(NSWindow* window) {
    void* key = (__bridge void*)window;
    if (!observed().insert(key).second) {
        return;
    }
    NSNotificationCenter* center = [NSNotificationCenter defaultCenter];
    for (NSNotificationName name :
         {NSWindowWillEnterFullScreenNotification, NSWindowWillExitFullScreenNotification}) {
        [center addObserverForName:name
                            object:window
                             queue:nil
                        usingBlock:^(NSNotification*) {
                          moving().insert(key);
                        }];
    }
    for (NSNotificationName name :
         {NSWindowDidEnterFullScreenNotification, NSWindowDidExitFullScreenNotification,
          NSWindowWillCloseNotification}) {
        [center addObserverForName:name
                            object:window
                             queue:nil
                        usingBlock:^(NSNotification*) {
                          moving().erase(key);
                        }];
    }
}

} // namespace

bool native_fullscreen(GLFWwindow* glfw_window) {
    NSWindow* window = glfwGetCocoaWindow(glfw_window);
    return ([window styleMask] & NSWindowStyleMaskFullScreen) != 0;
}

bool native_fullscreen_moving(GLFWwindow* glfw_window) {
    return moving().contains((__bridge void*)glfwGetCocoaWindow(glfw_window));
}

void toggle_native_fullscreen(GLFWwindow* glfw_window) {
    NSWindow* window = glfwGetCocoaWindow(glfw_window);
    observe(window);
    [window setCollectionBehavior:[window collectionBehavior] |
                                  NSWindowCollectionBehaviorFullScreenPrimary];
    moving().insert((__bridge void*)window);
    [window toggleFullScreen:nil];
}

} // namespace osc::platform
