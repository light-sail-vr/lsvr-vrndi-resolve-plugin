#ifdef __APPLE__

#import <AppKit/AppKit.h>
#import <Foundation/Foundation.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

#include "MacFileDialog.h"

#include <string.h>

bool mac_open_file_dialog(const char* message, const char* extension,
                          const char* initialPath, char* outPath, size_t outPathSize)
{
    if (!outPath || outPathSize == 0 || ![NSThread isMainThread]) {
        return false;
    }
    @autoreleasepool {
        NSOpenPanel* panel = [NSOpenPanel openPanel];
        panel.canChooseFiles = YES;
        panel.canChooseDirectories = NO;
        panel.allowsMultipleSelection = NO;
        if (message && *message) {
            panel.message = [NSString stringWithUTF8String:message];
        }
        if (extension && *extension) {
            if (@available(macOS 11.0, *)) {
                // ';'-separated list; an extension no app declares (.ilpd)
                // still gets a dynamic UTType, which filters correctly.
                NSMutableArray<UTType*>* types = [NSMutableArray array];
                NSArray<NSString*>* exts =
                    [[NSString stringWithUTF8String:extension] componentsSeparatedByString:@";"];
                for (NSString* ext in exts) {
                    UTType* type = ext.length ? [UTType typeWithFilenameExtension:ext] : nil;
                    if (type) {
                        [types addObject:type];
                    }
                }
                if (types.count > 0) {
                    panel.allowedContentTypes = types;
                }
            }
        }
        if (initialPath && *initialPath) {
            NSString* current = [NSString stringWithUTF8String:initialPath];
            NSString* dir = [current stringByDeletingLastPathComponent];
            if (dir.length > 0) {
                panel.directoryURL = [NSURL fileURLWithPath:dir isDirectory:YES];
            }
        }
        if ([panel runModal] != NSModalResponseOK || panel.URL == nil) {
            return false;
        }
        const char* picked = panel.URL.path.fileSystemRepresentation;
        if (!picked || strlen(picked) + 1 > outPathSize) {
            return false;
        }
        strlcpy(outPath, picked, outPathSize);
        return true;
    }
}

#endif // __APPLE__
