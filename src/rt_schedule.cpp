#include <rt_schedule.h>

#include <rt_path_schedule.h>

std::unique_ptr<IntegratorSchedule> createSchedule(IntegratorId id)
{
	switch (id) {
	case IntegratorId::Path:
		return std::make_unique<PathSchedule>();
	case IntegratorId::Count:
		break;
	}
	return nullptr;
}
