#include <rt_schedule.h>

#include <rt_bdpt_schedule.h>
#include <rt_light_schedule.h>
#include <rt_lvc_schedule.h>
#include <rt_path_schedule.h>

std::unique_ptr<IntegratorSchedule> createSchedule(IntegratorId id)
{
	switch (id) {
	case IntegratorId::Path:
		return std::make_unique<PathSchedule>();
	case IntegratorId::Light:
		return std::make_unique<LightSchedule>();
	case IntegratorId::Lvc:
		return std::make_unique<LvcSchedule>();
	case IntegratorId::Bdpt:
		return std::make_unique<BdptSchedule>();
	case IntegratorId::Count:
		break;
	}
	return nullptr;
}
