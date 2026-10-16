"""Frozen comparisons must catch field corruption that preserves placement counts."""
import hashlib
import json
from pathlib import Path
import re
import shutil
import struct
import unittest
from unittest.mock import patch
import uuid
import subprocess
import tempfile
from scripts.console_child import child_kwargs

from editor.tests.legacy_reference import (FROZEN, PLACEMENTS, buildings, compare_frozen,
    packet, reference_evidence, verify_frozen)


class LegacyLinkTests(unittest.TestCase):
    def test_undo_history_truncates_redo_and_preserves_save_positions(self):
        root = Path(__file__).resolve().parents[2]
        output = Path(tempfile.gettempdir()) / "mc2-el4-history" / uuid.uuid4().hex
        output.mkdir(parents=True)

        def fixture(header, implementation):
            source = r'''
#include <cassert>
#include <cstdlib>
#include <cstring>
#include <new>
#include <vector>
#define gosASSERT(x) assert(x)
'''
            if 'EList< Action*' in header:
                source += 'struct Heap { void* Malloc(size_t n) { return malloc(n); } void Free(void* p) { free(p); } };\n'
                source += 'Heap heap; Heap* systemHeap = &heap;\n'
                source += re.sub(r'^#include.*$', '', (root / 'mclib/elist.h').read_text(), flags=re.M)
            source += 'constexpr int MAX_DESC_LENGTH = 256;\n'
            source += header[header.index('class Action\n{'):header.index('struct VertexInfo')]
            source += header[header.index('class ActionUndoMgr\n{'):header.index('}; // class SActionMgr') + 2]
            source += implementation[implementation.index('ActionUndoMgr* ActionUndoMgr::instance'):implementation.index('bool ActionPaintTile::redo()')]
            return source + r'''
std::vector<int> visits;
struct Tracked : Action {
    int id;
    Tracked(int n, const char* name) : Action(name), id(n) {}
    ~Tracked() { visits.push_back(-id); }
    bool undo() override { visits.push_back(id * 10); return false; }
    bool redo() override { visits.push_back(id * 100); return false; }
};
int main() {
    ActionUndoMgr history;
    assert(!history.HaveUndo() && !history.HaveRedo());
    assert(history.GetCurrentPosition() == -1 && history.GetActionCount() == 0);
    assert(!history.GetUndoString() && !history.GetRedoString());
    assert(!history.ThereHasBeenANetChangeFromWhenLastSaved());
    assert(!strcmp(history.GetActionDescription(-1), ""));
    assert(!strcmp(history.GetActionDescription(0), ""));
    history.AddAction(new Tracked(1, "A"));
    history.AddAction(new Tracked(2, "B"));
    history.AddAction(new Tracked(3, "C"));
    assert(visits.empty());
    history.NoteThatASaveHasJustOccurred();
    assert(!history.ThereHasBeenANetChangeFromWhenLastSaved());
    assert(!history.Undo() && !history.Undo());
    assert((visits == std::vector<int>{30,20}));
    assert(!strcmp(history.GetUndoString(), "A") && !strcmp(history.GetRedoString(), "B"));
    assert(history.ThereHasBeenANetChangeFromWhenLastSaved());
    visits.clear();
    history.AddAction(new Tracked(4, "D"));
    assert((visits == std::vector<int>{-3,-2}));
    assert(history.GetActionCount() == 2 && history.GetCurrentPosition() == 1);
    assert(!history.HaveRedo() && !history.GetRedoString());
    assert(!strcmp(history.GetActionDescription(0), "A"));
    assert(!strcmp(history.GetActionDescription(1), "D"));
    assert(!strcmp(history.GetActionDescription(2), ""));
    assert(history.ThereHasBeenANetChangeFromWhenLastSaved());
    assert(!history.Undo() && !history.Undo());
    // Truncating the saved branch retains the legacy -1 save marker.
    assert(!history.ThereHasBeenANetChangeFromWhenLastSaved());
    assert(!history.HaveUndo() && history.HaveRedo());
    visits.clear(); history.Reset();
    assert((visits == std::vector<int>{-1,-4}));
    assert(history.GetActionCount() == 0 && history.GetCurrentPosition() == -1);
    assert(!history.ThereHasBeenANetChangeFromWhenLastSaved());
    history.AddAction(new Tracked(5, "E")); history.NoteThatASaveHasJustOccurred();
    history.AddAction(new Tracked(6, "F")); assert(!history.Undo());
    assert(!history.ThereHasBeenANetChangeFromWhenLastSaved());
    visits.clear(); history.AddAction(new Tracked(7, "G"));
    assert((visits == std::vector<int>{-6}));
    assert(history.ThereHasBeenANetChangeFromWhenLastSaved());
    assert(!history.Undo() && !history.ThereHasBeenANetChangeFromWhenLastSaved());
    assert(!history.Redo() && history.ThereHasBeenANetChangeFromWhenLastSaved());
    assert(!strcmp(history.GetUndoString(), "G"));
    history.NoteThatASaveHasJustOccurred();
    assert(!history.ThereHasBeenANetChangeFromWhenLastSaved());
    visits.clear(); history.Reset(); assert((visits == std::vector<int>{-5,-7}));
    history.Reset(); assert((visits == std::vector<int>{-5,-7}));
}
'''

        def run(source, label, succeeds=True):
            cpp = output / (label + '.cpp'); cpp.write_text(source)
            exe = output / (label + '.exe')
            built = subprocess.run(['clang++', '-std=c++23', str(cpp), '-o', str(exe)],
                                   capture_output=True, text=True, timeout=90, **child_kwargs(redirected=True))
            self.assertEqual(built.returncode, 0, built.stdout + built.stderr)
            result = subprocess.run([str(exe)], capture_output=True, timeout=30, **child_kwargs(redirected=True))
            self.assertEqual(result.returncode == 0, succeeds, result.stdout + result.stderr)

        paths = ('editor/Action.h', 'editor/Action.cpp')
        old = []
        for path in paths:
            result = subprocess.run(['git', 'show', '94faecce2:' + path], cwd=root,
                                    capture_output=True, text=True, timeout=30, **child_kwargs(redirected=True))
            self.assertEqual(result.returncode, 0, result.stderr)
            old.append(result.stdout)
        run(fixture(*old), 'old')
        source = fixture(*((root / path).read_text() for path in paths))
        run(source, 'current')
        if 'm_listUndoActions.pop_back();' in source:
            truncation = 'while (static_cast<long>(m_listUndoActions.size()) - 1 > m_CurrentPos)'
            self.assertIn(truncation, source)
            run(source.replace(truncation, 'while (false)'), 'broken-truncation', False)
            save = 'm_PosOfLastSave = -1;'
            start = source.index('void ActionUndoMgr::AddAction')
            end = source.index('void ActionUndoMgr::EmptyUndoList')
            wrong = source[:start] + source[start:end].replace(save, '') + source[end:]
            self.assertNotEqual(source, wrong)
            run(wrong, 'broken-save-position', False)
        print(f'History fixture artifacts: {output}')

    def test_campaign_bytes_nested_copies_and_first_last_moves(self):
        root = Path(__file__).resolve().parents[2]
        output = Path(tempfile.gettempdir()) / "mc2-el3-campaign" / uuid.uuid4().hex
        output.mkdir(parents=True)

        def method(source, signature):
            start = source.index(signature)
            opening = source.index("{", start)
            depth, end = 1, opening + 1
            while depth:
                depth += (source[end] == "{") - (source[end] == "}")
                end += 1
            return source[start:end] + "\n"

        def original(path):
            result = subprocess.run(['git', 'show', 'ab843f226:' + path], cwd=root,
                                    capture_output=True, text=True, timeout=30,
                                    **child_kwargs(redirected=True))
            self.assertEqual(result.returncode, 0, result.stderr)
            return result.stdout

        def fixture(header, campaign, dialog):
            legacy = 'public EList' in header
            source = r'''
#include <algorithm>
#include <cassert>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <format>
#include <string>
#include <vector>
#include "GameOS/include/mc2fmt.h"
#define gosASSERT(x) assert(x)
#define PAUSE(x) assert(false)
constexpr int NO_ERR = 0;
constexpr bool TRUE = true, FALSE = false;
using MemoryPtr = unsigned char*;
struct CString {
    std::string value;
    CString() = default;
    CString(const char* s) : value(s) {}
    char* GetBuffer(int = 0) { return value.data(); }
    bool IsEmpty() const { return value.empty(); }
    bool operator==(const CString&) const = default;
};
struct FitIniFile {
    std::ofstream file;
    int create(char* path) { file.open(path, std::ios::binary); file << "FITini \r\n"; return file ? 0 : 1; }
    void close() { file << "FITend \r\n"; file.close(); }
    long write(MemoryPtr data, size_t size) { file.write((char*)data, size); return size; }
    long writeBlock(const char*);
    long writeIdBoolean(const char*, bool);
    long writeIdLong(const char*, long);
    long writeIdString(const char*, const char*);
};
'''
            writer = (root / 'mclib/inifile.cpp').read_text()
            for name in ('writeBlock', 'writeIdBoolean', 'writeIdLong', 'writeIdString'):
                source += method(writer, 'long FitIniFile::' + name)
            if legacy:
                source += 'struct Heap { void* Malloc(size_t n) { return malloc(n); } void Free(void* p) { free(p); } };\n'
                source += 'Heap* systemHeap;\n'
                source += re.sub(r'^#include.*$', '', (root / 'mclib/elist.h').read_text(), flags=re.M)
            source += re.sub(r'^#include.*$', '', header, flags=re.M)
            for signature in ('CMissionData::CMissionData()', 'bool CMissionData::operator==',
                              'bool CMissionData::Save', 'CGroupData::CGroupData()',
                              'bool CGroupData::operator==', 'bool CGroupData::Save',
                              'CCampaignData::CCampaignData()', 'bool CCampaignData::operator==',
                              'bool CCampaignData::Save'):
                source += method(campaign, signature)
            source += r'''
struct CListBox { int selected = 0; int GetCurSel() { return selected; } void SetCurSel(int n) { selected = n; } };
void setGroupListBoxValues(CListBox&, const CGroupList&) {}
struct CCampaignDialog {
    CCampaignData m_CampaignData;
    CListBox m_GroupListControl;
    void UpdateData(bool) {}
    void OnCaMoveUpButton(); void OnCaMoveDownButton();
};
'''
            for name in ('OnCaMoveUpButton', 'OnCaMoveDownButton'):
                source += method(dialog, 'void CCampaignDialog::' + name)
            main = r'''
int main(int argc, char** argv) {
    assert(argc == 2);
    CCampaignDialog dialog;
    auto& campaign = dialog.m_CampaignData;
    campaign.m_Name = "Order fixture";
    campaign.m_CBills = 54321;
    campaign.m_FinalVideo = "final.bik";
    for (auto label : {"A", "B", "C"}) {
        CGroupData group; group.m_Label = label;
        group.m_ABLScript = "script";
        for (auto filename : {"mc2_24", "mc2_01", "mc2_24", "mc2_03"}) {
            CMissionData mission; mission.m_MissionFile = filename;
            mission.m_IsMandatory = false;
            group.m_MissionList.APPEND(mission);
        }
        campaign.m_GroupList.APPEND(group);
    }
    const CCampaignData snapshot = campaign;
    campaign.m_GroupList[0].m_MissionList[0].m_MissionFile = "changed";
    assert(snapshot.m_GroupList[0].m_MissionList[0].m_MissionFile == CString("mc2_24"));
    campaign = snapshot;
    auto save = [&](const char* suffix) {
        CString path; path.value = std::string(argv[1]) + suffix; assert(campaign.Save(path));
    };
    auto order = [&](const char* a, const char* b, const char* c, int selected) {
        assert(campaign.m_GroupList[0].m_Label == CString(a));
        assert(campaign.m_GroupList[1].m_Label == CString(b));
        assert(campaign.m_GroupList[2].m_Label == CString(c));
        assert(dialog.m_GroupListControl.selected == selected);
    };
    save("-null.fit");
    dialog.m_GroupListControl.selected = 0; dialog.OnCaMoveUpButton(); order("A","B","C",0);
    dialog.OnCaMoveDownButton(); order("B","A","C",1); save("-first-down.fit");
    dialog.OnCaMoveUpButton(); order("A","B","C",0);
    dialog.m_GroupListControl.selected = 2; dialog.OnCaMoveDownButton(); order("A","B","C",2);
    dialog.OnCaMoveUpButton(); order("A","C","B",1); save("-last-up.fit");
    dialog.OnCaMoveDownButton(); order("A","B","C",2); save("-restored.fit");
    assert(campaign == snapshot);
}
'''
            if legacy:
                main = main.replace('    CCampaignDialog dialog;', '    Heap heap; systemHeap = &heap;\n    CCampaignDialog dialog;')
            return source + main.replace('APPEND', 'Append' if legacy else 'push_back')

        def run(source, label, succeeds=True):
            cpp = output / (label + '.cpp'); cpp.write_text(source)
            exe = output / (label + '.exe')
            result = subprocess.run(['clang++', '-std=c++23', '-I', str(root), str(cpp), '-o', str(exe)],
                                    capture_output=True, text=True, timeout=90, **child_kwargs(redirected=True))
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            result = subprocess.run([str(exe), str(output / label)], capture_output=True, timeout=30,
                                    **child_kwargs(redirected=True))
            if succeeds:
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            else:
                self.assertNotEqual(result.returncode, 0)

        paths = ('editor/campaignData.h', 'editor/CampaignData.cpp', 'editor/CampaignDialog.cpp')
        run(fixture(*(original(p) for p in paths)), 'old')
        source = fixture(*((root / p).read_text() for p in paths))
        run(source, 'current')
        for suffix in ('null', 'first-down', 'last-up', 'restored'):
            self.assertEqual((output / f'old-{suffix}.fit').read_bytes(),
                             (output / f'current-{suffix}.fit').read_bytes())
        self.assertEqual((output / 'current-null.fit').read_bytes(), (output / 'current-restored.fit').read_bytes())
        if 'public EList' not in (root / paths[0]).read_text():
            for member in ('m_GroupList', 'm_MissionList'):
                wrong = source.replace(member + '.begin()', member + '.rbegin()').replace(member + '.end()', member + '.rend()')
                self.assertNotEqual(source, wrong)
                label = 'wrong-save-order-' + member
                run(wrong, label)
                self.assertNotEqual((output / 'current-null.fit').read_bytes(), (output / (label + '-null.fit')).read_bytes())
            for direction, offset in (('up', '- 1'), ('down', '+ 1')):
                expression = f'm_GroupList[selectedItemIndex {offset}]'
                self.assertIn(expression, source)
                run(source.replace(expression, 'm_GroupList[selectedItemIndex]'), 'wrong-move-' + direction, False)
        print(f'Campaign fixture artifacts: {output}')

    def test_child_order_snapshots_and_reverse_undo(self):
        root = Path(__file__).resolve().parents[2]
        output = root / "scratch/el2-links" / uuid.uuid4().hex
        output.mkdir(parents=True)
        def method(source, signature):
            start = source.index(signature)
            opening = source.index("{", start)
            depth = 1
            end = opening + 1
            while depth:
                depth += (source[end] == "{") - (source[end] == "}")
                end += 1
            return source[start:end] + "\n"
        building = (root / "editor/BuildingLink.cpp").read_text()
        brush = (root / "editor/LinkBrush.cpp").read_text()
        header = (root / "editor/BuildingLink.h").read_text()
        header = re.sub(r'^#include.*$', '', header, flags=re.M)
        nested = (root / "editor/LinkBrush.h").read_text()
        nested = nested[nested.index("\tstruct LinkInfo"):nested.index("\tLinkAction* pAction;")]
        source = r'''
#include <algorithm>
#include <cassert>
#include <vector>
#define gosASSERT(x) ((void)0)
#define SPEW(x) ((void)0)
namespace Stuff { struct Vector3D { float x{}, y{}, z{}; bool operator==(const Vector3D&) const = default; }; }
'''
        source += r'''
struct EditorObject {
    int id, alignment; Stuff::Vector3D pos;
    int getID() const { return id; } auto getPosition() const { return pos; }
    int getAlignment() const { return alignment; }
    void setAlignment(int a) { alignment = a; }
};
struct Terrain { float getTerrainElevation(const Stuff::Vector3D&) { return 0; } };
Terrain* land;
'''
        source += header + '\nstruct Action {};\nclass LinkBrush { public:\n' + nested + '\n};\n'
        source += r'''
struct EditorObjectMgr {
    std::vector<EditorObject> objects;
    std::vector<BuildingLink*> links;
    std::vector<int> visits;
    static EditorObjectMgr* active;
    static auto instance() { return active; }
    EditorObject* getObjectAtLocation(float x, float y) {
        for (auto& o : objects) if (o.pos.x == x && o.pos.y == y) return &o;
        return nullptr;
    }
    BuildingLink* getLinkWithParent(const EditorObject* o) {
        for (auto* l : links) if (l->HasParent(o)) return l;
        return nullptr;
    }
    BuildingLink* getLinkWithBuilding(const EditorObject* o) {
        for (auto* l : links) if (l->HasParent(o) || l->HasChild(o)) return l;
        return nullptr;
    }
    void deleteLink(BuildingLink* l) {
        visits.push_back(-static_cast<int>(l->GetParentPosition().x));
        links.erase(std::find(links.begin(), links.end(), l)); delete l;
    }
    void addLink(BuildingLink* l) { visits.push_back(static_cast<int>(l->GetParentPosition().x)); links.push_back(l); }
    ~EditorObjectMgr() { for (auto* l : links) delete l; }
};
EditorObjectMgr* EditorObjectMgr::active;
'''
        for signature in ('BuildingLink::BuildingLink( const EditorObject*',
                          'BuildingLink::BuildingLink( const Stuff::Vector3D&',
                          'BuildingLink::BuildingLink( const BuildingLink&',
                          'BuildingLink& BuildingLink::operator=', 'void BuildingLink::CopyData',
                          'bool BuildingLink::AddChild', 'int BuildingLink::GetLinkCount',
                          'const Stuff::Vector3D& BuildingLink::GetParentPosition',
                          'bool BuildingLink::HasChild', 'bool BuildingLink::HasParent',
                          'bool BuildingLink::RemoveObject', 'int BuildingLink::GetChildrenPositions',
                          'void\tBuildingLink::FixHeights'):
            source += method(building, signature)
        for signature in ('void LinkBrush::LinkAction::AddToListOnce', 'LinkBrush::LinkAction::LinkAction',
                          'bool LinkBrush::LinkAction::undo', 'bool LinkBrush::LinkAction::redo',
                          'LinkBrush::LinkInfo::LinkInfo'):
            source += method(brush, signature)
        source += r'''
int main() {
    EditorObjectMgr mgr; EditorObjectMgr::active = &mgr;
    mgr.objects = {{1, 1, {1,0,0}}, {2, 2, {2,0,0}}, {3, 0, {3,0,0}}, {4, 0, {4,0,0}}};
    auto& a = mgr.objects[0]; auto& b = mgr.objects[1]; auto& c = mgr.objects[2]; auto& d = mgr.objects[3];
    BuildingLink link(&a);
    assert(link.AddChild(&d)); assert(link.AddChild(&c)); assert(!link.AddChild(&d));
    Stuff::Vector3D positions[3]{{99,0,0}};
    assert(link.GetChildrenPositions(positions, 1) == 2 && positions[0].x == 99);
    assert(link.GetChildrenPositions(positions, 3) == 2 && positions[0] == d.pos && positions[1] == c.pos);
    BuildingLink copy(link); BuildingLink assigned(&b); assigned = link; assigned = assigned;
    assert(!link.RemoveObject(&a)); assert(link.RemoveObject(&d)); assert(!link.RemoveObject(&d));
    assert(link.AddChild(&d));
    assert(link.GetChildrenPositions(positions, 3) == 2 && positions[0] == c.pos && positions[1] == d.pos);
    for (auto* snapshot : {&copy, &assigned}) {
        assert(snapshot->GetChildrenPositions(positions, 3) == 2 && positions[0] == d.pos && positions[1] == c.pos);
    }
    assert(link.RemoveObject(&c)); assert(link.RemoveObject(&d)); assert(link.GetLinkCount() == 0);
    assert(link.GetChildrenPositions(positions, 3) == 0);
    EditorObject elevated = d; elevated.pos.z = 1;
    assert(link.AddChild(&d)); assert(link.AddChild(&c)); assert(link.AddChild(&elevated));
    Terrain terrain; land = &terrain; link.FixHeights();
    assert(link.RemoveObject(&d));
    assert(link.GetChildrenPositions(positions, 3) == 2 && positions[0] == c.pos && positions[1] == d.pos);
    LinkBrush::LinkAction empty; assert(empty.undo()); assert(empty.redo());
    BuildingLink first(&a); first.AddChild(&c); first.AddChild(&d);
    BuildingLink second(&b); second.AddChild(&c);
    LinkBrush::LinkAction action;
    action.AddToListOnce({&first, LinkBrush::LinkInfo::REMOVE});
    action.AddToListOnce({&second, LinkBrush::LinkInfo::REMOVE});
    BuildingLink duplicate(&a);
    action.AddToListOnce({&duplicate, LinkBrush::LinkInfo::ADD});
    assert(action.undo()); assert((mgr.visits == std::vector<int>{2,1}));
    assert(c.alignment == 1 && d.alignment == 1);
    assert(mgr.links[1]->GetChildrenPositions(positions, 3) == 2 && positions[0] == c.pos && positions[1] == d.pos);
    mgr.visits.clear(); assert(action.redo()); assert((mgr.visits == std::vector<int>{-2,-1})); assert(mgr.links.empty());
    mgr.visits.clear(); assert(action.undo()); assert((mgr.visits == std::vector<int>{2,1}));
    LinkBrush::LinkAction edit; edit.AddToListOnce({mgr.links[1]});
    mgr.links[1]->RemoveObject(&c);
    assert(edit.undo()); assert(mgr.links[1]->GetChildrenPositions(positions, 3) == 2 && positions[0] == c.pos);
    assert(edit.redo()); assert(mgr.links[1]->GetChildrenPositions(positions, 3) == 1 && positions[0] == d.pos);
}
'''
        def run(contents, label):
            cpp = output / (label + '.cpp'); cpp.write_text(contents)
            exe = output / (label + '.exe')
            built = subprocess.run(['clang++', '-std=c++23', '-I', str(root), str(cpp), '-o', str(exe)],
                                   capture_output=True, text=True, timeout=90, **child_kwargs(redirected=True))
            self.assertEqual(built.returncode, 0, built.stdout + built.stderr)
            result = subprocess.run([str(exe)], capture_output=True, timeout=30, **child_kwargs(redirected=True))
            if label == 'links':
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            return result.returncode
        self.assertEqual(run(source, 'links'), 0)
        # Mutate the production bodies, keeping the fixture's expected order fixed.
        append = 'children.push_back( tmp );'
        self.assertIn(append, source)
        self.assertNotEqual(run(source.replace(append, 'children.insert(children.begin(), tmp);'), 'wrong-child-order'), 0)
        reverse = 'changedLinks.rbegin(); iter != changedLinks.rend(); ++iter'
        self.assertIn(reverse, source)
        self.assertNotEqual(run(source.replace(reverse, 'changedLinks.begin(); iter != changedLinks.end(); ++iter'), 'wrong-undo-order'), 0)
        print(f'Link fixture artifacts: {output}')


class FrozenParityTests(unittest.TestCase):
    def setUp(self):
        self.root = Path(__file__).resolve().parents[2] / "scratch/e5b1-negative" / uuid.uuid4().hex
        self.root.mkdir(parents=True)
        for path in FROZEN.iterdir():
            shutil.copyfile(path, self.root / path.name)
        self.evidence = reference_evidence(verify_frozen())

    def corrupt_unit(self, family, key, replacement):
        row = verify_frozen()["placements"][family]
        prefix = next(iter(row["part"])).split("/", 1)[0]
        pilot = row["part"][prefix + "/Pilot"][0].split(" ", 1)[1]
        section = "Warrior" + pilot if key in ("Profile", "Brain", "NumCells", "NumStaticVars") else prefix
        path = self.root / f"e5_reference_{family}.fit"
        contents = path.read_bytes()
        start = contents.index(("[" + section + "]").encode())
        end = contents.find(b"\n[", start + 1)
        if end < 0:
            end = len(contents)
        body = contents[start:end]
        mutated, count = re.subn(rb"(?m)^(\w+(?:\[\d+\])?\s+" + key.encode() + rb"\s*=\s*)[^\r\n]*",
                                lambda m: m[1] + replacement.encode(), body)
        self.assertEqual(count, 1)
        path.write_bytes(contents[:start] + mutated + contents[end:])

    def test_frozen_capture_self_compare_and_raw_packet_storage(self):
        self.assertIn("9/9", compare_frozen(self.root, self.evidence)["frozenComparison"])
        for family in PLACEMENTS:
            path = self.root / f"e5_reference_{family}.pak"
            data = path.read_bytes()
            table = struct.unpack_from("<I", data, 4)[0]
            count = table // 4 - 2
            rewritten = bytearray(data[:table])
            for index in range(count):
                struct.pack_into("<I", rewritten, 8 + 4 * index, len(rewritten))
                rewritten.extend(packet(data, index))
            path.write_bytes(rewritten)
        compare_frozen(self.root, self.evidence)

    def test_fresh_baseline_save_guid_keeps_content_exact(self):
        path = self.root / "e5_reference_before.pak"
        original = path.read_bytes()
        changed = original[:-16] + uuid.uuid4().bytes_le
        self.assertNotEqual(changed, original)
        path.write_bytes(changed)
        compare_frozen(self.root, self.evidence)
        # A fresh save identity cannot excuse terrain or tactical-map changes.
        for index in (0, 2):
            with self.subTest(packet=index):
                corrupted = bytearray(changed)
                offset = struct.unpack_from("<I", corrupted, 8 + 4 * index)[0] & 0x1fffffff
                corrupted[offset] ^= 1
                path.write_bytes(corrupted)
                with self.assertRaisesRegex(ValueError, "baseline PAK differs"):
                    compare_frozen(self.root, self.evidence)
        for corrupted in (changed[:-1], changed + b"extra"):
            path.write_bytes(corrupted)
            with self.assertRaisesRegex(ValueError, "16-byte GUID"):
                compare_frozen(self.root, self.evidence)
        path.write_bytes(changed)
        fit = self.root / "e5_reference_before.fit"
        fit.write_bytes(fit.read_bytes().replace(b"\r\n", b"\n"))
        with self.assertRaisesRegex(ValueError, "baseline FIT differs"):
            compare_frozen(self.root, self.evidence)

    def test_complete_unit_values_same_count_controls(self):
        for family in ("mech", "vehicle"):
            for key, replacement in (("VariantNumber", "1"), ("Damage", "1.000000"), ("BaseColor", "0"),
                ("HighlightColor1", "0"), ("HighlightColor2", "0"), ("SelfRepairBehavior", "0"),
                ("ObjectProfile", '"other-profile"'), ("Profile", '"other-pilot"'), ("Brain", '"OtherBrain"'),
                ("NumStaticVars", "1"), ("SquadNum", "108"), ("AlternativeStartIndex", "36"),
                ("ControlDataType", "3"), ("Rotation", "1.000000")):
                with self.subTest(family=family, key=key):
                    self.corrupt_unit(family, key, replacement)
                    with self.assertRaises(ValueError):
                        compare_frozen(self.root, self.evidence)
                    shutil.copyfile(FROZEN / f"e5_reference_{family}.fit", self.root / f"e5_reference_{family}.fit")

    def test_complete_building_records_same_count_controls(self):
        for family in PLACEMENTS.keys() - {"mech", "vehicle"}:
            path = self.root / f"e5_reference_{family}.pak"
            original = path.read_bytes()
            for field in range(10):
                with self.subTest(family=family, field=field):
                    data = bytearray(original)
                    table = struct.unpack_from("<I", data, 4)[0]
                    count = table // 4 - 2
                    records = buildings(data)
                    raw = bytearray(packet(data, 1))
                    offset = 4 + (len(records) - 1) * 40 + field * 4
                    struct.pack_into("<I", raw, offset, struct.unpack_from("<I", raw, offset)[0] ^ 0x40000000)
                    output = bytearray(data[:table])
                    for index in range(count):
                        struct.pack_into("<I", output, 8 + 4 * index, len(output))
                        output.extend(raw if index == 1 else packet(data, index))
                    path.write_bytes(output)
                    with self.assertRaises(ValueError):
                        compare_frozen(self.root, self.evidence)
                    path.write_bytes(original)

    def test_extra_cells_and_missing_fields_are_not_presence_parity(self):
        path = self.root / "e5_reference_mech.fit"
        content = path.read_bytes()
        path.write_bytes(content.replace(b"FITend", b"[Warrior4Cell0]\r\nl Cell = 2\r\nl MemType = 1\r\nf Value = 150\r\nFITend"))
        with self.assertRaisesRegex(ValueError, "complete Part/Warrior/cell"):
            compare_frozen(self.root, self.evidence)

    def test_frozen_fingerprints_and_freeze_refusal(self):
        manifest = self.root / "placements.json"
        manifest.write_bytes(manifest.read_bytes() + b" ")
        with self.assertRaisesRegex(ValueError, "manifest fingerprint"):
            verify_frozen(self.root)
        from editor.tests.legacy_reference import retain
        with self.assertRaises(FileExistsError):
            retain(FROZEN, FROZEN, {})
        self.assertEqual(hashlib.sha256((FROZEN / "placements.json").read_bytes()).hexdigest(),
                         "3e3ed40dfb5c057fadf4d88a2ee36c128335b78bedfa28a745eeb24bb2e5eb37")
        from scripts.run_editor_smoke import main
        with patch("sys.argv", ["run_editor_smoke.py", "--freeze-legacy-reference", str(FROZEN)]), patch("scripts.run_editor_smoke.nofocus.spawn") as launch:
            with self.assertRaises(SystemExit) as stopped:
                main()
            self.assertEqual(stopped.exception.code, 2)
            launch.assert_not_called()

    def test_marker_fit_values_same_count_control(self):
        path = self.root / "e5_reference_marker.fit"
        content = path.read_bytes()
        start = content.index(b"[NavMarker1]")
        end = content.index(b"[NavMarker2]")
        body = content[start:end].replace(b"radius = 256.000000", b"radius = 257.000000")
        self.assertNotEqual(body, content[start:end])
        path.write_bytes(content[:start] + body + content[end:])
        with self.assertRaisesRegex(ValueError, "marker FIT"):
            compare_frozen(self.root, self.evidence)

    def test_owned_marker_legacy_reload_control_requires_loader_save_and_equal_records(self):
        from scripts.run_editor_smoke import run_owned_marker_reload
        deploy, report = self.root / "deploy", self.root / "reload-report"
        report.mkdir()
        source = self.root / "e5_reference_marker"
        def accepted(*args):
            directory = deploy / "terrain_gen_out/owned_marker_reload"
            for extension in ("fit", "pak"):
                shutil.copyfile(source.with_suffix("." + extension), directory / ("reloaded." + extension))
            return {"passed": True, "captured": "", "name": "owned_marker_reload"}
        with patch("scripts.run_editor_smoke.run_case_with_retry", side_effect=accepted) as launch:
            result = run_owned_marker_reload(Path("editor.exe"), deploy, source, report, 3, 180)
            self.assertTrue(result["passed"])
            self.assertIn("legacy loader", result["detail"])
            self.assertEqual(launch.call_args.args[-1], "saved")
            self.assertIn("-mission=terrain_gen_out/owned_marker_reload/input.pak", launch.call_args.args[-2])
        def corrupted(*args):
            result = accepted(*args)
            path = deploy / "terrain_gen_out/owned_marker_reload/reloaded.fit"
            path.write_bytes(path.read_bytes().replace(b"radius = 256.000000", b"radius = 257.000000"))
            return result
        with patch("scripts.run_editor_smoke.run_case_with_retry", side_effect=corrupted):
            self.assertFalse(run_owned_marker_reload(Path("editor.exe"), deploy, source, report, 3, 180)["passed"])
        with patch("scripts.run_editor_smoke.run_case_with_retry", return_value={"passed": False, "bucket": "autoload_missing"}):
            self.assertFalse(run_owned_marker_reload(Path("editor.exe"), deploy, source, report, 3, 180)["passed"])
        shutil.copyfile(FROZEN / "e5_reference_before.fit", source.with_suffix(".fit"))
        with patch("scripts.run_editor_smoke.run_case_with_retry") as launch:
            with self.assertRaisesRegex(ValueError, "one inserted FIT"):
                run_owned_marker_reload(Path("editor.exe"), deploy, source, report, 3, 180)
            launch.assert_not_called()

    def test_owned_objective_reload_uses_proven_panel_fixture(self):
        from scripts.run_editor_smoke import run_owned_objective_reload
        source = self.root / "objective-ResourcePoints-edited"
        before_fit = (FROZEN / "e5_reference_before.fit").read_bytes()
        prefix, section, remaining = before_fit.partition(b"[Team0Objective1]")
        edited_fit = prefix + section + re.sub(rb"ResourcePoints = \d+", b"ResourcePoints = 17", remaining, count=1)
        self.assertNotEqual(before_fit, edited_fit)
        for name in ("input.fit", "fixture.fit"):
            (self.root / name).write_bytes(before_fit)
        source.with_suffix(".fit").write_bytes(edited_fit)
        legacy_pak = (FROZEN / "e5_reference_before.pak").read_bytes()
        fixture_pak = legacy_pak + b"panel stock fixture"
        self.assertNotEqual(fixture_pak, legacy_pak)
        for name in ("input.pak", "fixture.pak", source.with_suffix(".pak").name):
            (self.root / name).write_bytes(fixture_pak)
        gate = dict(result=dict(name="objectives_panel", passed=True),
                    provenance=dict(input_pak_sha256=hashlib.sha256(fixture_pak).hexdigest(),
                                    input_fit_sha256=hashlib.sha256(before_fit).hexdigest()))
        (self.root / "gate.json").write_text(json.dumps(gate), encoding="utf-8")
        deploy, report = self.root / "deploy", self.root / "report"
        report.mkdir()

        def accepted(*args):
            directory = deploy / "terrain_gen_out/owned_objective_reload"
            for extension in ("fit", "pak"):
                shutil.copyfile(source.with_suffix("." + extension), directory / ("reloaded." + extension))
            return dict(name="owned_objective_reload", passed=True)

        def reload():
            return run_owned_objective_reload(Path("editor.exe"), deploy, source, report, 3, 180)

        with patch("scripts.run_editor_smoke.run_case_with_retry", side_effect=accepted) as launch:
            self.assertTrue(reload()["passed"])
            launch.assert_called_once()
            self.assertEqual(launch.call_args.args[-1], "saved")

        def corrupted(*args):
            row = accepted(*args)
            (deploy / "terrain_gen_out/owned_objective_reload/reloaded.fit").write_bytes(before_fit)
            return row

        with patch("scripts.run_editor_smoke.run_case_with_retry", side_effect=corrupted):
            self.assertEqual(reload()["bucket"], "owned_objective_reload_changed")
        with patch("scripts.run_editor_smoke.run_case_with_retry", return_value=dict(passed=False, bucket="autoload_missing")):
            self.assertFalse(reload()["passed"])
        with patch("scripts.run_editor_smoke.run_case_with_retry") as launch:
            source.with_suffix(".fit").write_bytes(before_fit)
            with self.assertRaisesRegex(ValueError, "scalar saved value wrong"):
                reload()
            source.with_suffix(".fit").write_bytes(edited_fit)
            source.with_suffix(".pak").write_bytes(legacy_pak)
            with self.assertRaisesRegex(ValueError, "control changed the PAK"):
                reload()
            source.with_suffix(".pak").write_bytes(fixture_pak)
            for name in ("fixture.pak", "input.pak"):
                (self.root / name).write_bytes(legacy_pak)
                with self.assertRaisesRegex(ValueError, "does not match proven panel input"):
                    reload()
                (self.root / name).write_bytes(fixture_pak)
            gate["result"]["passed"] = False
            (self.root / "gate.json").write_text(json.dumps(gate), encoding="utf-8")
            with self.assertRaisesRegex(ValueError, "does not match proven panel input"):
                reload()
            launch.assert_not_called()

    def test_owned_marker_reload_requires_clean_current_lane3_executable(self):
        from scripts.run_editor_smoke import marker_reload_provenance
        exe = Path("A:/Games/mc2-opengl/mc2-win64-lane3/Mission Editor.exe").resolve()
        head = "0123456789abcdef"
        with patch("scripts.run_editor_smoke.subprocess.check_output", return_value=head), patch.object(Path, "read_bytes", return_value=b"editor"):
            for fingerprint, observed in (({"present": False, "reason": "banner-absent"}, '"reason": "banner-absent"'),
                ({"present": True, "sha12": "old", "dirty": False}, '"sha12": "old"'),
                ({"present": True, "sha12": head[:12], "dirty": True}, '"dirty": true')):
                with patch("scripts.run_editor_smoke.exe_fingerprint.extract", return_value=fingerprint):
                    with self.assertRaisesRegex(ValueError, "clean build fingerprint") as failure:
                        marker_reload_provenance(exe)
                    detail = str(failure.exception)
                    self.assertIn(exe.as_posix(), detail)
                    self.assertIn("expected_sha=" + head[:12], detail)
                    self.assertIn(observed, detail)
                    self.assertIn("rebuild EditRel", detail)
            with patch("scripts.run_editor_smoke.exe_fingerprint.extract", return_value={"present": True, "sha12": head[:12], "dirty": False}):
                self.assertEqual(marker_reload_provenance(exe)["head"], head)
        with self.assertRaisesRegex(ValueError, "restricted to lane3"):
            marker_reload_provenance(self.root / "editor.exe")

    def test_enemy_cells_same_count_and_remapped_player_controls(self):
        from editor.tests.run_document_proofs import grade_player_defaults
        fields = {"Part30/TeamID": ["c 0"], "Part30/CommanderID": ["c 2"], "Part30/PlayerPart": ["b FALSE"],
            "Part30/ControlDataType": ["ul 1"], "Part30/ObjectProfile": ['st "PM101100"'],
            "Warrior30/Profile": ['st "PMWGreen"'], "Warrior30/Brain": ['st "DredAttack01"'],
            "Warrior30/NumCells": ["l 3"], "Warrior30/NumStaticVars": ["l 0"]}
        for index, (number, kind, value) in enumerate(((2, 1, 150), (9, 0, 0), (28, 0, 0))):
            fields.update({f"Warrior30Cell{index}/Cell": [f"l {number}"], f"Warrior30Cell{index}/MemType": [f"l {kind}"],
                           f"Warrior30Cell{index}/Value": [f"f {value}"]})
        grade_player_defaults(fields, 2, 53, "PMWGreen")
        for key in fields:
            with self.subTest(key=key):
                corrupted = dict(fields)
                kind, value = fields[key][0].split(" ", 1)
                corrupted[key] = [kind + ' "other"' if kind == "st" else kind + " 7"]
                with self.assertRaisesRegex(ValueError, "policy values"):
                    grade_player_defaults(corrupted, 2, 53, "PMWGreen")
        with self.assertRaises(ValueError):
            grade_player_defaults(fields, 0, 53, "PMWGreen")

    def test_owned_writer_preserves_existing_cursors_and_cells(self):
        from editor.tests.legacy_reference import preserve_authored_units
        before = {"Part1/AlternativeStartIndex": ["ul 35"], "Warrior1Cell0/Value": ["f 150"]}
        preserve_authored_units(before, {**before, "Part30/ObjectNumber": ["ul 53"]})
        for key in before:
            with self.assertRaisesRegex(ValueError, "authored"):
                preserve_authored_units(before, {**before, key: ["ul 99"]})
        with self.assertRaisesRegex(ValueError, "authored"):
            preserve_authored_units(before, {**before, "Warrior1/Extra": ["l 1"]})


if __name__ == "__main__":
    unittest.main()
