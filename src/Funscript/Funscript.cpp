#include "Funscript.h"

#include "SDL.h"
#include "OpenFunscripterUtil.h"
#include "EventSystem.h"
#include "OFS_Serialization.h"
#include "OpenFunscripter.h"

#include <algorithm>
#include <limits>
#include <set>

Funscript::Funscript() 
{
	NotifyActionsChanged();
	saveMutex = SDL_CreateMutex();
}

Funscript::~Funscript()
{
	SDL_DestroyMutex(saveMutex);
}

void Funscript::setBaseScript(nlohmann::json& base)
{
	// the point of BaseLoaded is to not wipe any attributes added by other tools
	// BaseLoaded is supposed to hold everything that isn't generated by OFS
	BaseLoaded = base;
	BaseLoaded.erase("actions");
	BaseLoaded.erase("rawActions");
	BaseLoaded.erase("version");
	BaseLoaded.erase("inverted");
	BaseLoaded.erase("range");
	BaseLoaded.erase("OpenFunscripter");
	BaseLoaded.erase("metadata");
}

void Funscript::setScriptTemplate() noexcept
{
	// setup a base funscript template
	Json = nlohmann::json(BaseLoaded);
	Json["actions"] = nlohmann::json::array();
	Json["rawActions"] = nlohmann::json::array();
	Json["version"] = "1.0";
	Json["inverted"] = false;
	Json["range"] = 90; // I think this is mostly ignored anyway
	Json["OpenFunscripter"] = nlohmann::json::object();
}

void Funscript::NotifyActionsChanged() noexcept
{
	funscriptChanged = true;
}

void Funscript::NotifySelectionChanged() noexcept
{
	selectionChanged = true;
}

void Funscript::loadMetadata() noexcept
{
	if (Json.contains("metadata")) {
		auto& meta = Json["metadata"];
		OFS::serializer::load(&metadata, &meta);
	}
}

void Funscript::saveMetadata() noexcept
{
	OFS::serializer::save(&metadata, &Json["metadata"]);
}

void Funscript::loadSettings() noexcept
{
	if (Json.contains("OpenFunscripter")) {
		auto& settings = Json["OpenFunscripter"];
		scriptSettings.player = &OpenFunscripter::ptr->player.settings;
		OFS::serializer::load(&scriptSettings, &settings);

		OFS::unpacker upkg(&Json["OpenFunscripter"]);
		OFS_REFLECT_NAMED(Recordings, rawData.Recordings, upkg);
		
		// fix action position in the array -> index == frame_no
		std::vector<Funscript::FunscriptRawData::Recording> Fixed;
		for (auto&& rec : rawData.Recordings) {
			Funscript::FunscriptRawData::Recording recording;
			for (auto&& raw : rec.RawActions) {
				if (raw.frame_no > 0) {
					recording.RawActions.resize(raw.frame_no + 1);
					recording.RawActions[raw.frame_no] = raw;
				}
			}
			Fixed.emplace_back(std::move(recording));
		}
		rawData.Recordings = std::move(Fixed);
	}
}

void Funscript::saveSettings() noexcept
{
	scriptSettings.player = &OpenFunscripter::ptr->player.settings;
	OFS::serializer::save(&scriptSettings, &Json["OpenFunscripter"]);

	// filter all empty actions to serialize
	std::vector<Funscript::FunscriptRawData::Recording> filtered;
	for (auto&& rec : rawData.Recordings) {
		Funscript::FunscriptRawData::Recording filteredRecording;
		filteredRecording.RawActions.reserve(rec.RawActions.size());
		for (auto&& raw : rec.RawActions) {
			if (raw.frame_no > 0) {
				filteredRecording.RawActions.emplace_back(raw);
			}
		}
		filtered.emplace_back(std::move(filteredRecording));
	}
	OFS::archiver ar(&Json["OpenFunscripter"]);
	OFS_REFLECT_NAMED(Recordings, filtered, ar);
}

void Funscript::update() noexcept
{
	if (funscriptChanged) {
		funscriptChanged = false;
		SDL_Event ev;
		ev.type = EventSystem::FunscriptActionsChangedEvent;
		SDL_PushEvent(&ev);

		// TODO: find out how expensive this is on an already sorted array
		sortActions(data.Actions);
	}
	if (selectionChanged) {
		selectionChanged = false;
		SDL_Event ev;
		ev.type = EventSystem::FunscriptSelectionChangedEvent;
		SDL_PushEvent(&ev);
	}
}

bool Funscript::open(const std::string& file)
{
	current_path = file;
	scriptOpened = true;

	nlohmann::json json;
	json = Util::LoadJson(file);
	if (!json.is_object() && json["actions"].is_array()) {
		LOGF_ERROR("Failed to parse funscript. \"%s\"", file.c_str());
		return false;
	}

	setBaseScript(json);
	Json = json;
	auto actions = json["actions"];
	data.Actions.clear();

	std::set<FunscriptAction> actionSet;
	if (actions.is_array()) {
		for (auto& action : actions) {
			int32_t time_ms = action["at"];
			int32_t pos = action["pos"];
			if (time_ms >= 0) {
				actionSet.emplace(time_ms, pos);
			}
		}
	}
	data.Actions.assign(actionSet.begin(), actionSet.end());

	loadSettings();
	loadMetadata();

	if (metadata.title.empty()) {
		metadata.title = std::filesystem::path(current_path)
			.replace_extension("")
			.filename()
			.string();
	}

	NotifyActionsChanged();
	return true;
}

void Funscript::save(const std::string& path, bool override_location)
{
	if (override_location) {
		current_path = path;
	}
	struct SaveThreadData {
		nlohmann::json jsonObj;
		std::string path;
		SDL_mutex* mutex;
	};
	SaveThreadData* threadData;
	threadData = new SaveThreadData();
	threadData->mutex = saveMutex;
	threadData->path = path;

	setScriptTemplate();
	saveSettings();
	saveMetadata();

	auto& actions = Json["actions"];
	actions.clear();

	// make sure actions are sorted
	sortActions(data.Actions);

	for (auto& action : data.Actions) {
		// a little validation just in case
		if (action.at < 0)
			continue;

		nlohmann::json actionObj = {
			{ "at", action.at },
			{ "pos", Util::Clamp<int32_t>(action.pos, 0, 100) }
		};
		actions.emplace_back(actionObj);
	}

	threadData->jsonObj = std::move(Json); // give ownership to the thread
	auto thread = [](void* user) -> int {
		SaveThreadData* data = static_cast<SaveThreadData*>(user);
		SDL_LockMutex(data->mutex);
		Util::WriteJson(data->jsonObj, data->path.c_str());
		SDL_UnlockMutex(data->mutex);
		delete data;
		return 0;
	};
	auto handle = SDL_CreateThread(thread, "SaveScriptThread", threadData);
	SDL_DetachThread(handle);
}

float Funscript::GetPositionAtTime(int32_t time_ms) noexcept
{
	if (data.Actions.size() == 0) {	return 0; } 
	else if (data.Actions.size() == 1) return data.Actions[0].pos;

	for (int i = 0; i < data.Actions.size()-1; i++) {
		auto& action = data.Actions[i];
		auto& next = data.Actions[i + 1];
		
		if (time_ms > action.at && time_ms < next.at) {
			// interpolate position
			int32_t last_pos = action.pos;
			float diff = next.pos - action.pos;
			float progress = (float)(time_ms - action.at) / (next.at - action.at);
			float interp = last_pos +(progress * (float)diff);
			return interp;
		}
		else if (action.at == time_ms) {
			return action.pos;
		}

	}

	return data.Actions.back().pos;
}

float Funscript::GetRawPositionAtFrame(int32_t frame_no) noexcept
{
	auto& recording = rawData.Active();
	if (frame_no >= recording.RawActions.size()) return 0;
	// this is stupid
	auto pos = recording.RawActions[frame_no].pos;
	if (pos >= 0) { 
		return pos; 
	}
	else if((frame_no + 1) < recording.RawActions.size()) {
		pos = recording.RawActions[frame_no + 1].pos;
		if (pos >= 0) {
			return pos;
		}
		else if((frame_no - 1) >= 0) {
			pos = recording.RawActions[frame_no - 1].pos;
			if (pos >= 0) {
				return pos;
			}
		}
	}
	return 0;
}

FunscriptAction* Funscript::getAction(FunscriptAction action) noexcept
{
	auto it = std::find(data.Actions.begin(), data.Actions.end(), action);
	if (it != data.Actions.end())
		return &(*it);
	return nullptr;
}

FunscriptAction* Funscript::getActionAtTime(std::vector<FunscriptAction>& actions, int32_t time_ms, uint32_t max_error_ms) noexcept
{
	// gets an action at a time with a margin of error
	int32_t smallest_error = std::numeric_limits<int32_t>::max();
	FunscriptAction* smallest_error_action = nullptr;

	for (int i = 0; i < actions.size(); i++) {
		auto& action = actions[i];
		
		if (action.at > (time_ms + (max_error_ms/2)))
			break;

		int32_t error = std::abs(time_ms - action.at);
		if (error <= max_error_ms) {
			if (error <= smallest_error) {
				smallest_error = error;
				smallest_error_action = &action;
			}
			else {
				break;
			}
		}
	}
	return smallest_error_action;
}

FunscriptAction* Funscript::getNextActionAhead(int32_t time_ms) noexcept
{
	auto it = std::find_if(data.Actions.begin(), data.Actions.end(), 
		[&](auto& action) {
			return action.at > time_ms;
	});

	if (it != data.Actions.end())
		return &(*it);

	return nullptr;
}

FunscriptAction* Funscript::getPreviousActionBehind(int32_t time_ms) noexcept
{
	auto it = std::find_if(data.Actions.rbegin(), data.Actions.rend(),
		[&](auto& action) {
			return action.at < time_ms;
		});
	
	if (it != data.Actions.rend())
		return &(*it);

	return nullptr;
}

bool Funscript::EditAction(FunscriptAction oldAction, FunscriptAction newAction) noexcept
{
	// update action
	auto act = getAction(oldAction);
	if (act != nullptr) {
		act->at = newAction.at;
		act->pos = newAction.pos;
		checkForInvalidatedActions();
		NotifyActionsChanged();
		return true;
	}
	return false;
}

void Funscript::AddEditAction(FunscriptAction action, float frameTimeMs) noexcept
{
	auto close = getActionAtTime(data.Actions, action.at, frameTimeMs);
	if (close != nullptr) {
		*close = action;
	}
	else {
		AddAction(action);
	}
}

void Funscript::PasteAction(FunscriptAction paste, int32_t error_ms) noexcept
{
	auto act = GetActionAtTime(paste.at, error_ms);
	if (act != nullptr) {
		RemoveAction(*act);
	}
	AddAction(paste);
	NotifyActionsChanged();
}

void Funscript::checkForInvalidatedActions() noexcept
{
	auto it = std::remove_if(data.selection.begin(), data.selection.end(), [&](auto& selected) {
		auto found = getAction(selected);
		if (found == nullptr)
			return true;
		return false;
	});
	if (it != data.selection.end()) {
		data.selection.erase(it);
		NotifySelectionChanged();
	}
}

void Funscript::RemoveAction(FunscriptAction action, bool checkInvalidSelection) noexcept
{
	auto it = std::find(data.Actions.begin(), data.Actions.end(), action);
	if (it != data.Actions.end()) {
		data.Actions.erase(it);
		NotifyActionsChanged();

		if (checkInvalidSelection) { checkForInvalidatedActions(); }
	}
}

void Funscript::RemoveActions(const std::vector<FunscriptAction>& removeActions) noexcept
{
	for (auto& action : removeActions)
		RemoveAction(action, false);
	NotifyActionsChanged();
}

void Funscript::RangeExtendSelection(int32_t rangeExtend) noexcept
{
	auto ExtendRange = [](std::vector<FunscriptAction*>& actions, int32_t rangeExtend) -> void {
		if (rangeExtend == 0) { return; }
		if (actions.size() < 0) { return; }

		auto StretchPosition = [](int32_t position, int32_t lowest, int32_t highest, int extension) -> int32_t
		{
			int32_t newHigh = Util::Clamp<int32_t>(highest + extension, 0, 100);
			int32_t newLow = Util::Clamp<int32_t>(lowest - extension, 0, 100);

			double relativePosition = (position - lowest) / (double)(highest - lowest);
			double newposition = relativePosition * (newHigh - newLow) + newLow;

			return Util::Clamp<int32_t>(newposition, 0, 100);
		};

		int lastExtremeIndex = 0;
		int32_t lastValue = (*actions[0]).pos;
		int32_t lastExtremeValue = lastValue;

		int32_t lowest = lastValue;
		int32_t highest = lastValue;

		enum class direction {
			NONE,
			UP,
			DOWN
		};
		direction strokeDir = direction::NONE;

		for (int index = 0; index < actions.size(); index++)
		{
			// Direction unknown
			if (strokeDir == direction::NONE)
			{
				if ((*actions[index]).pos < lastExtremeValue) {
					strokeDir = direction::DOWN;
				}
				else if ((*actions[index]).pos > lastExtremeValue) {
					strokeDir = direction::UP;
				}
			}
			else
			{
				if (((*actions[index]).pos < lastValue && strokeDir == direction::UP)     //previous was highpoint
					|| ((*actions[index]).pos > lastValue && strokeDir == direction::DOWN) //previous was lowpoint
					|| (index == actions.size() - 1))                            //last action
				{
					for (int i = lastExtremeIndex + 1; i < index; i++)
					{
						FunscriptAction& action = *actions[i];
						action.pos = StretchPosition(action.pos, lowest, highest, rangeExtend);
					}

					lastExtremeValue = (*actions[index - 1]).pos;
					lastExtremeIndex = index - 1;

					highest = lastExtremeValue;
					lowest = lastExtremeValue;

					strokeDir = (strokeDir == direction::UP) ? direction::DOWN : direction::UP;
				}

				lastValue = (*actions[index]).pos;
				if (lastValue > highest)
					highest = lastValue;
				if (lastValue < lowest)
					lowest = lastValue;
			}
		}
	};
	std::vector<FunscriptAction*> rangeExtendSelection;
	rangeExtendSelection.reserve(SelectionSize());
	int selectionOffset = 0;
	for (auto&& act : data.Actions) {
		for (int i = selectionOffset; i < data.selection.size(); i++) {
			if (data.selection[i] == act) {
				rangeExtendSelection.push_back(&act);
				selectionOffset = i;
				break;
			}
		}
	}
	ClearSelection();
	ExtendRange(rangeExtendSelection, rangeExtend);
}

bool Funscript::ToggleSelection(FunscriptAction action) noexcept
{
	auto it = std::find(data.selection.begin(), data.selection.end(), action);
	bool is_selected = it != data.selection.end();
	if (is_selected) {
		data.selection.erase(it);
	}
	else {
		data.selection.emplace_back(action);
	}
	NotifySelectionChanged();
	return !is_selected;
}

void Funscript::SetSelection(FunscriptAction action, bool selected) noexcept
{
	auto it = std::find(data.selection.begin(), data.selection.end(), action);
	bool is_selected = it != data.selection.end();
	if(is_selected && !selected)
	{
		data.selection.erase(it);
	}
	else if(!is_selected && selected) {
		data.selection.emplace_back(action);
	}
	NotifySelectionChanged();
}

void Funscript::SelectTopActions()
{
	if (data.selection.size() < 3) return;
	std::vector<FunscriptAction> deselect;
	for (int i = 1; i < data.selection.size() - 1; i++) {
		auto& prev = data.selection[i - 1];
		auto& current = data.selection[i];
		auto& next = data.selection[i + 1];

		auto& min1 = prev.pos < current.pos ? prev : current;
		auto& min2 = min1.pos < next.pos ? min1 : next;
		deselect.emplace_back(min1);
		if (min1.at != min2.at)
			deselect.emplace_back(min2);

	}
	for (auto& act : deselect)
		SetSelection(act, false);
	NotifySelectionChanged();
}

void Funscript::SelectBottomActions()
{
	if (data.selection.size() < 3) return;
	std::vector<FunscriptAction> deselect;
	for (int i = 1; i < data.selection.size() - 1; i++) {
		auto& prev = data.selection[i - 1];
		auto& current = data.selection[i];
		auto& next = data.selection[i + 1];

		auto& max1 = prev.pos > current.pos ? prev : current;
		auto& max2 = max1.pos > next.pos ? max1 : next;
		deselect.emplace_back(max1);
		if (max1.at != max2.at)
			deselect.emplace_back(max2);

	}
	for (auto& act : deselect)
		SetSelection(act, false);
	NotifySelectionChanged();
}

void Funscript::SelectMidActions()
{
	if (data.selection.size() < 3) return;
	auto selectionCopy = data.selection;
	SelectTopActions();
	auto topPoints = data.selection;
	data.selection = selectionCopy;
	SelectBottomActions();
	auto bottomPoints = data.selection;

	selectionCopy.erase(std::remove_if(selectionCopy.begin(), selectionCopy.end(),
		[&](auto val) {
			return std::any_of(topPoints.begin(), topPoints.end(), [&](auto a) { return a == val; })
				|| std::any_of(bottomPoints.begin(), bottomPoints.end(), [&](auto a) { return a == val; });
		}), selectionCopy.end());
	data.selection = selectionCopy;
	sortSelection();
	NotifySelectionChanged();
}

void Funscript::SelectTime(int32_t from_ms, int32_t to_ms, bool clear) noexcept
{
	if(clear)
		ClearSelection();

	for (auto& action : data.Actions) {
		if (action.at >= from_ms && action.at <= to_ms) {
			ToggleSelection(action);
		}
		else if (action.at > to_ms)
			break;
	}

	if (!clear)
		sortSelection();
	NotifySelectionChanged();
}

void Funscript::SelectAction(FunscriptAction select) noexcept
{
	auto action = GetAction(select);
	if (action != nullptr) {
		if (ToggleSelection(select)) {
			// keep selection ordered for rendering purposes
			sortSelection();
		}
		NotifySelectionChanged();
	}
}

void Funscript::DeselectAction(FunscriptAction deselect) noexcept
{
	auto action = GetAction(deselect);
	if (action != nullptr)
		SetSelection(*action, false);
	NotifySelectionChanged();
}

void Funscript::SelectAll() noexcept
{
	ClearSelection();
	data.selection.assign(data.Actions.begin(), data.Actions.end());
	NotifySelectionChanged();
}

void Funscript::RemoveSelectedActions() noexcept
{
	RemoveActions(data.selection);
	ClearSelection();
	//NotifyActionsChanged(); // already called in RemoveActions
	NotifySelectionChanged();
}

void Funscript::moveActionsTime(std::vector<FunscriptAction*> moving, int32_t time_offset)
{
	ClearSelection();
	for (auto move : moving) {
		move->at += time_offset;
	}
	NotifyActionsChanged();
}

void Funscript::moveActionsPosition(std::vector<FunscriptAction*> moving, int32_t pos_offset)
{
	ClearSelection();
	for (auto move : moving) {
		move->pos += pos_offset;
		move->pos = Util::Clamp(move->pos, 0, 100);
	}
	NotifyActionsChanged();
}

void Funscript::MoveSelectionTime(int32_t time_offset) noexcept
{
	if (!HasSelection()) return;
	std::vector<FunscriptAction*> moving;

	// faster path when everything is selected
	if (data.selection.size() == data.Actions.size()) {
		for (auto& action : data.Actions)
			moving.push_back(&action);
		moveActionsTime(moving, time_offset);
		SelectAll();
		return;
	}

	auto prev = GetPreviousActionBehind(data.selection.front().at);
	auto next = GetNextActionAhead(data.selection.back().at);

	int32_t min_bound = 0;
	int32_t max_bound = std::numeric_limits<int32_t>::max();

	float frameTime = OpenFunscripter::ptr->player.getFrameTimeMs();
	if (time_offset > 0) {
		if (next != nullptr) {
			max_bound = next->at - frameTime;
			time_offset = std::min(time_offset, max_bound - data.selection.back().at);
		}
	}
	else
	{
		if (prev != nullptr) {
			min_bound = prev->at + frameTime;
			time_offset = std::max(time_offset, min_bound - data.selection.front().at);
		}
	}

	for (auto& find : data.selection) {
		auto m = getAction(find);
		if(m != nullptr)
			moving.push_back(m);
	}

	ClearSelection();
	for (auto move : moving) {
		move->at += time_offset;
		data.selection.emplace_back(*move);
	}
	NotifyActionsChanged();
}

void Funscript::MoveSelectionPosition(int32_t pos_offset) noexcept
{
	if (!HasSelection()) return;
	std::vector<FunscriptAction*> moving;
	
	// faster path when everything is selected
	if (data.selection.size() == data.Actions.size()) {
		for (auto& action : data.Actions)
			moving.push_back(&action);
		moveActionsPosition(moving, pos_offset);
		SelectAll();
		return;
	}

	for (auto& find : data.selection) {
		auto m = getAction(find);
		if (m != nullptr)
			moving.push_back(m);
	}

	ClearSelection();
	for (auto move : moving) {
		move->pos += pos_offset;
		move->pos = Util::Clamp(move->pos, 0, 100);
		data.selection.emplace_back(*move);
	}
	NotifyActionsChanged();
}

void Funscript::EqualizeSelection() noexcept
{
	if (data.selection.size() < 3) return;
	sortSelection(); // might be unnecessary
	auto first = data.selection.front();
	auto last = data.selection.back();
	float duration = last.at - first.at;
	int32_t step_ms = std::round(duration / (float)(data.selection.size()-1));
		
	auto copySelection = data.selection;
	RemoveSelectedActions(); // clears selection

	for (int i = 1; i < copySelection.size()-1; i++) {
		auto& newAction = copySelection[i];
		newAction.at = first.at + (i * step_ms);
	}

	for (auto& action : copySelection)
		AddAction(action);

	data.selection = std::move(copySelection);
}

void Funscript::InvertSelection() noexcept
{
	if (data.selection.size() == 0) return;
	auto copySelection = data.selection;
	RemoveSelectedActions();
	for (auto& act : copySelection)
	{
		act.pos = std::abs(act.pos - 100);
		AddAction(act);
	}
	data.selection = copySelection;
}

void Funscript::AlignWithFrameTimeSelection(float frameTimeMs) noexcept
{
	if (data.selection.size() == 0) return;
	auto copySelection = data.selection;
	RemoveSelectedActions();
	for (auto& act : copySelection)
	{
		float offset = std::fmod<float>(act.at, frameTimeMs);
		act.at -= (int)offset;
		AddAction(act);
	}
	data.selection = copySelection;
}
