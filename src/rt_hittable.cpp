#include <rt_hittable.h>

#include <imgui.h>

bool sphere::params()
{
	//the sibling project used SliderScalar with a fixed 0.001-2 radius / -1-1 position range,
	//which cannot represent its own default scene: the ground sphere is radius 100 at y=-100.5,
	//and a slider clamps a value into range the moment it is touched, so selecting the ground
	//sphere and nudging the slider silently collapsed it. Drag for position (unbounded) and a
	//logarithmic slider for radius keep the same controls without that trap
	double rMin = 0.001;
	double rMax = 200.0;
	bool changed = ImGui::SliderScalar("radius", ImGuiDataType_Double, &radius, &rMin, &rMax, "%.3f", ImGuiSliderFlags_Logarithmic);

	double speed = 0.01;
	changed |= ImGui::DragScalarN("position", ImGuiDataType_Double, &center, 3, (float)speed, nullptr, nullptr, "%.3f");

	return changed;
}

bool sphere::hit(const ray& r, double t_min, double t_max, hit_record& rec) const
{
	glm::dvec3 oc = r.origin() - center;
	double a = glm::dot(r.direction(), r.direction());
	double half_b = glm::dot(oc, r.direction());
	double c = glm::dot(oc, oc) - radius * radius;

	auto discriminant = half_b * half_b - a * c;
	if (discriminant < 0) return false;
	auto sqrtd = sqrt(discriminant);

	// Find the nearest root that lies in the acceptable range.
	auto root = (-half_b - sqrtd) / a;
	if (root < t_min || t_max < root) {
		root = (-half_b + sqrtd) / a;
		if (root < t_min || t_max < root)
			return false;
	}

	rec.t = root;
	rec.p = r.at(rec.t);
	glm::dvec3 outward_normal = (rec.p - center) / radius;
	rec.set_face_normal(r, outward_normal);
	rec.mat_ptr = mat_ptr.get();

	return true;
}
