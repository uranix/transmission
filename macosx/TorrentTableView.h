/******************************************************************************
 * $Id$
 *
 * Copyright (c) 2005-2008 Transmission authors and contributors
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 *****************************************************************************/

#import <Cocoa/Cocoa.h>
#import <transmission.h>
#import <Controller.h>

@class TorrentCell;

#define GROUP_SEPARATOR_HEIGHT 18.0

@interface TorrentTableView : NSOutlineView
{
    IBOutlet Controller * fController;
    
    TorrentCell * fTorrentCell;
    
    NSUserDefaults * fDefaults;
    
    NSMutableIndexSet * fCollapsedGroups;
    
    IBOutlet NSMenu * fContextRow, * fContextNoRow;
    
    int fMouseControlRow, fMouseRevealRow, fMouseActionRow, fActionPushedRow;
    NSArray * fSelectedValues;
    
    IBOutlet NSMenu * fActionMenu, * fUploadMenu, * fDownloadMenu, * fRatioMenu;
    Torrent * fMenuTorrent;
    
    float fPiecesBarPercent;
    NSTimer * fPiecesBarTimer;
}

- (BOOL) isGroupCollapsed: (int) value;
- (void) removeCollapsedGroup: (int) value;
- (void) removeAllCollapsedGroups;

- (void) removeButtonTrackingAreas;
- (void) setControlButtonHover: (int) row;
- (void) setRevealButtonHover: (int) row;
- (void) setActionButtonHover: (int) row;

- (void) selectValues: (NSArray *) values;
- (NSArray *) selectedValues;
- (NSArray *) selectedTorrents;

- (void) toggleControlForTorrent: (Torrent *) torrent;

- (void) displayTorrentMenuForEvent: (NSEvent *) event;

- (void) setQuickLimitMode: (id) sender;
- (void) setQuickLimit: (id) sender;

- (void) setQuickRatioMode: (id) sender;
- (void) setQuickRatio: (id) sender;

- (void) checkFile: (id) sender;

- (void) togglePiecesBar;
- (float) piecesBarPercent;

@end
