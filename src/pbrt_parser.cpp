#include <pbrt_parser.h>

#include <cmath>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <sstream>

#include <fmt/format.h>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <zlib.h>

namespace {

//---------------------------------------------------------------- reading files

bool hasExtension(const std::filesystem::path& path, std::string_view extension)
{
	const std::string name = path.filename().string();
	return name.size() >= extension.size() && name.compare(name.size() - extension.size(), extension.size(), extension) == 0;
}

// the whole file, through zlib for a .gz name as pbrt reads one (ReadDecompressedFileContents)
std::optional<std::string> readFileText(const std::filesystem::path& path)
{
	if (hasExtension(path, ".gz")) {
		gzFile file = gzopen(path.string().c_str(), "rb");
		if (file == nullptr) {
			return std::nullopt;
		}
		std::string text;
		char buffer[1 << 16];
		int read = 0;
		while ((read = gzread(file, buffer, sizeof(buffer))) > 0) {
			text.append(buffer, (size_t)read);
		}
		const bool ok = read == 0;
		gzclose(file);
		return ok ? std::optional(std::move(text)) : std::nullopt;
	}
	std::ifstream file(path, std::ios::binary);
	if (!file) {
		return std::nullopt;
	}
	std::ostringstream text;
	text << file.rdbuf();
	return text.str();
}

//---------------------------------------------------------------- the scanner

struct Token {
	std::string_view text;
	PbrtLoc loc;
};

// pbrt's Tokenizer: splits the text into words, numbers, quoted strings, brackets and comments
class Scanner {
public:
	Scanner(std::string text, uint32_t file)
		: m_text(std::move(text))
	{
		m_loc.file = file;
		m_loc.line = 1;
		m_pos = m_text.data();
		m_end = m_pos + m_text.size();
	}

	// the next token, or nullopt at the end of the text or at an error, which sets `error`
	std::optional<Token> next(std::string& error)
	{
		while (true) {
			const char* start = m_pos;
			const PbrtLoc startLoc = m_loc;
			int ch = get();
			if (ch == EOF) {
				return std::nullopt;
			} else if (ch == ' ' || ch == '\n' || ch == '\t' || ch == '\r') {
				continue;
			} else if (ch == '"') {
				//to the closing quote; a backslash escapes the character after it
				bool escaped = false;
				while ((ch = get()) != '"') {
					if (ch == EOF) {
						error = "premature EOF";
						m_errorLoc = startLoc;
						return std::nullopt;
					} else if (ch == '\n') {
						error = "unterminated string";
						m_errorLoc = startLoc;
						return std::nullopt;
					} else if (ch == '\\') {
						escaped = true;
						if (get() == EOF) {
							error = "premature EOF";
							m_errorLoc = startLoc;
							return std::nullopt;
						}
					}
				}
				if (!escaped) {
					return Token { { start, (size_t)(m_pos - start) }, startLoc };
				}
				m_unescaped.clear();
				for (const char* p = start; p < m_pos; p++) {
					if (*p != '\\') {
						m_unescaped.push_back(*p);
						continue;
					}
					p++;
					switch (*p) {
					case 'b': m_unescaped.push_back('\b'); break;
					case 'f': m_unescaped.push_back('\f'); break;
					case 'n': m_unescaped.push_back('\n'); break;
					case 'r': m_unescaped.push_back('\r'); break;
					case 't': m_unescaped.push_back('\t'); break;
					case '\\': m_unescaped.push_back('\\'); break;
					case '\'': m_unescaped.push_back('\''); break;
					case '"': m_unescaped.push_back('"'); break;
					default:
						error = fmt::format("unexpected escaped character \"{}\"", *p);
						m_errorLoc = startLoc;
						return std::nullopt;
					}
				}
				return Token { m_unescaped, startLoc };
			} else if (ch == '[' || ch == ']') {
				return Token { { start, 1 }, startLoc };
			} else if (ch == '#') {
				//a comment runs to the end of the line
				while ((ch = get()) != EOF) {
					if (ch == '\n' || ch == '\r') {
						unget();
						break;
					}
				}
				return Token { { start, (size_t)(m_pos - start) }, startLoc };
			} else {
				//a directive name or a number: up to whitespace, a quote or a bracket
				while ((ch = get()) != EOF) {
					if (ch == ' ' || ch == '\n' || ch == '\t' || ch == '\r' || ch == '"' || ch == '[' || ch == ']') {
						unget();
						break;
					}
				}
				return Token { { start, (size_t)(m_pos - start) }, startLoc };
			}
		}
	}

	PbrtLoc errorLoc() const { return m_errorLoc; }

private:
	int get()
	{
		if (m_pos == m_end) {
			return EOF;
		}
		const int ch = *m_pos++;
		if (ch == '\n') {
			m_loc.line++;
		}
		return ch;
	}

	void unget()
	{
		m_pos--;
		if (*m_pos == '\n') {
			m_loc.line--;
		}
	}

	std::string m_text;
	const char* m_pos { nullptr };
	const char* m_end { nullptr };
	PbrtLoc m_loc;
	PbrtLoc m_errorLoc;
	// a string with escapes, unescaped; valid until the next token
	std::string m_unescaped;
};

bool isQuoted(std::string_view token)
{
	return token.size() >= 2 && token.front() == '"' && token.back() == '"';
}

std::string_view dequote(std::string_view token)
{
	return token.substr(1, token.size() - 2);
}

//---------------------------------------------------------------- transforms

// pbrt's LookAt(): cameraFromWorld for a camera at `pos` looking at `look`. Its x axis is up x dir, which is what
// makes pbrt's world left-handed
glm::dmat4 pbrtLookAt(const glm::dvec3& pos, const glm::dvec3& look, const glm::dvec3& up, bool& degenerate)
{
	const glm::dvec3 dir = glm::normalize(look - pos);
	const glm::dvec3 rightRaw = glm::cross(glm::normalize(up), dir);
	degenerate = glm::length(rightRaw) == 0.0;
	if (degenerate) {
		return glm::dmat4(1.0);
	}
	const glm::dvec3 right = glm::normalize(rightRaw);
	const glm::dvec3 newUp = glm::cross(dir, right);
	glm::dmat4 worldFromCamera(1.0);
	worldFromCamera[0] = glm::dvec4(right, 0.0);
	worldFromCamera[1] = glm::dvec4(newUp, 0.0);
	worldFromCamera[2] = glm::dvec4(dir, 0.0);
	worldFromCamera[3] = glm::dvec4(pos, 1.0);
	return glm::inverse(worldFromCamera);
}

//---------------------------------------------------------------- the graphics state

constexpr int START_TRANSFORM = 1;
constexpr int END_TRANSFORM = 2;
constexpr int ALL_TRANSFORMS = 3;

// BasicSceneBuilder::GraphicsState: what AttributeBegin saves and AttributeEnd restores
struct GraphicsState {
	// the current transform at the shutter's open and close
	glm::dmat4 ctm[2] { glm::dmat4(1.0), glm::dmat4(1.0) };
	int activeTransforms { ALL_TRANSFORMS };
	bool reverseOrientation { false };
	PbrtMaterialRef material { 0, {} };
	// the AreaLightSource in effect, with its own location; empty type for none
	PbrtEntity areaLight;
	std::string insideMedium;
	std::string outsideMedium;
	std::string colorSpace { "srgb" };
	// Attribute "shape" | "light" | "material" | "medium" | "texture": parameters every later directive of that
	// kind takes unless it gives its own
	std::vector<PbrtParam> shapeAttributes;
	std::vector<PbrtParam> lightAttributes;
	std::vector<PbrtParam> materialAttributes;
	std::vector<PbrtParam> mediumAttributes;
	std::vector<PbrtParam> textureAttributes;

	template <typename F>
	void forActiveTransforms(F apply)
	{
		for (int i = 0; i < 2; i++) {
			if (activeTransforms & (1 << i)) {
				ctm[i] = apply(ctm[i]);
			}
		}
	}
};

// a directive's parameters, followed by the attributes it inherits that it does not give itself
PbrtParams withAttributes(std::vector<PbrtParam> params, const std::vector<PbrtParam>& attributes)
{
	PbrtParams result;
	result.list = std::move(params);
	for (const PbrtParam& attribute : attributes) {
		bool given = false;
		for (const PbrtParam& p : result.list) {
			given = given || p.name == attribute.name;
		}
		if (!given) {
			result.list.push_back(attribute);
		}
	}
	return result;
}

class Parser {
public:
	Parser(PbrtScene& scene, const PbrtParseOptions& options)
		: m_scene(scene)
		, m_options(options)
	{
		//BasicSceneBuilder's constructor: a "diffuse" material every shape has until it is given another
		PbrtEntity defaultMaterial;
		defaultMaterial.type = "diffuse";
		m_scene.materials.push_back(std::move(defaultMaterial));
	}

	bool run(std::unique_ptr<Scanner> first, std::string& error)
	{
		m_files.push_back(File { std::move(first), false, 0 });
		while (true) {
			std::optional<Token> token = nextToken(false);
			if (m_failed) {
				break;
			}
			if (!token.has_value()) {
				break;
			}
			//an Import that ended while the last directive looked ahead for parameters: its state goes now, after
			//that directive has used it
			for (; m_pendingRestores > 0; m_pendingRestores--) {
				restoreImport();
			}
			directive(*token);
			if (m_failed) {
				break;
			}
		}
		if (!m_failed) {
			finish();
		}
		error = m_error;
		return !m_failed;
	}

private:
	// a file being read. An Import restores the graphics state when its file ends: pbrt reads it into a copy of the
	// scene builder, so nothing it changes in the state reaches the importing file
	struct File {
		std::unique_ptr<Scanner> scanner;
		bool restoresState;
		size_t stackDepth;
	};

	// the Import's state goes, and with it anything it left open
	void restoreImport()
	{
		m_stack.resize(m_importDepths.back());
		m_importDepths.pop_back();
		m_state = m_importSaved.back();
		m_importSaved.pop_back();
	}

	void fail(const PbrtLoc& loc, const std::string& message)
	{
		if (!m_failed) {
			m_failed = true;
			m_error = fmt::format("{}: {}", m_scene.where(loc), message);
		}
	}

	void warn(const PbrtLoc& loc, std::string message)
	{
		m_scene.warnings.push_back({ loc, std::move(message) });
	}

	// the next token of the current file, else of the file that included it; comments are dropped. `required` turns
	// the end of every file into an error
	std::optional<Token> nextToken(bool required)
	{
		if (m_unget.has_value()) {
			return std::exchange(m_unget, std::nullopt);
		}
		while (!m_files.empty()) {
			std::string scanError;
			std::optional<Token> token = m_files.back().scanner->next(scanError);
			if (!scanError.empty()) {
				fail(m_files.back().scanner->errorLoc(), scanError);
				return std::nullopt;
			}
			if (!token.has_value()) {
				File& ended = m_files.back();
				if (ended.restoresState) {
					m_importDepths.push_back(ended.stackDepth);
					m_pendingRestores++;
				}
				//the scanner owns the text earlier tokens view; keep it until the parse ends
				m_finished.push_back(std::move(ended.scanner));
				m_files.pop_back();
				continue;
			}
			if (token->text[0] == '#') {
				continue;
			}
			return token;
		}
		if (required) {
			fail(m_lastLoc, "premature end of file");
		}
		return std::nullopt;
	}

	std::optional<Token> required()
	{
		std::optional<Token> token = nextToken(true);
		if (token.has_value()) {
			m_lastLoc = token->loc;
		}
		return token;
	}

	std::optional<double> number()
	{
		std::optional<Token> token = required();
		if (!token.has_value()) {
			return std::nullopt;
		}
		return parseNumber(*token);
	}

	std::optional<double> parseNumber(const Token& token)
	{
		const std::string text(token.text);
		char* end = nullptr;
		const double value = std::strtod(text.c_str(), &end);
		if (end == text.c_str() || *end != '\0') {
			fail(token.loc, fmt::format("\"{}\": expected a number", text));
			return std::nullopt;
		}
		return value;
	}

	std::optional<std::string> quoted()
	{
		std::optional<Token> token = required();
		if (!token.has_value()) {
			return std::nullopt;
		}
		if (!isQuoted(token->text)) {
			fail(token->loc, fmt::format("\"{}\": expected quoted string", token->text));
			return std::nullopt;
		}
		return std::string(dequote(token->text));
	}

	// n numbers in brackets, as Transform and ConcatTransform take
	bool bracketedNumbers(const Token& directive, double* out, int n)
	{
		std::optional<Token> open = required();
		if (!open.has_value() || open->text != "[") {
			fail(directive.loc, fmt::format("{}: expected '['", directive.text));
			return false;
		}
		for (int i = 0; i < n; i++) {
			std::optional<double> v = number();
			if (!v.has_value()) {
				return false;
			}
			out[i] = *v;
		}
		std::optional<Token> close = required();
		if (!close.has_value() || close->text != "]") {
			fail(directive.loc, fmt::format("{}: expected ']'", directive.text));
			return false;
		}
		return true;
	}

	// pbrt's parseParameters(): "type name" followed by one value or a bracketed list, until a token that is not a
	// quoted string
	std::vector<PbrtParam> parameters()
	{
		std::vector<PbrtParam> result;
		while (true) {
			std::optional<Token> token = nextToken(false);
			if (m_failed || !token.has_value()) {
				return result;
			}
			if (!isQuoted(token->text)) {
				m_unget = token;
				return result;
			}
			PbrtParam param;
			param.loc = token->loc;
			std::istringstream declaration { std::string(dequote(token->text)) };
			declaration >> param.type >> param.name;
			if (param.type.empty()) {
				fail(token->loc, fmt::format("parameter \"{}\" doesn't have a type declaration", dequote(token->text)));
				return result;
			}
			if (param.name.empty()) {
				fail(token->loc, fmt::format("unable to find parameter name from \"{}\"", dequote(token->text)));
				return result;
			}
			if (param.type == "point") {
				param.type = "point3";
			} else if (param.type == "vector") {
				param.type = "vector3";
			} else if (param.type == "color") {
				param.type = "rgb";
			} else if (param.type == "normal3") {
				param.type = "normal";
			}

			auto addValue = [&](const Token& value) {
				if (isQuoted(value.text)) {
					param.strings.emplace_back(dequote(value.text));
				} else if (value.text == "true") {
					param.bools.push_back(true);
				} else if (value.text == "false") {
					param.bools.push_back(false);
				} else if (std::optional<double> v = parseNumber(value)) {
					param.numbers.push_back(*v);
				}
			};
			std::optional<Token> value = required();
			if (!value.has_value()) {
				return result;
			}
			if (value->text == "[") {
				while (true) {
					value = required();
					if (!value.has_value() || m_failed) {
						return result;
					}
					if (value->text == "]") {
						break;
					}
					addValue(*value);
				}
			} else {
				addValue(*value);
			}
			if (m_failed) {
				return result;
			}
			const int kinds = (param.numbers.empty() ? 0 : 1) + (param.strings.empty() ? 0 : 1) + (param.bools.empty() ? 0 : 1);
			if (kinds > 1) {
				fail(param.loc, fmt::format("parameter \"{}\" mixes numbers, strings and bools", param.name));
				return result;
			}
			//pbrt-v3 wrote bools as quoted strings
			if (param.type == "bool" && !param.strings.empty()) {
				for (const std::string& s : param.strings) {
					if (s != "true" && s != "false") {
						fail(param.loc, fmt::format("{}: neither \"true\" nor \"false\" in bool parameter list", s));
						return result;
					}
					param.bools.push_back(s == "true");
				}
				param.strings.clear();
			}
			result.push_back(std::move(param));
		}
	}

	// a directive that takes a quoted type and parameters
	std::optional<PbrtEntity> entity(const Token& directive, const std::vector<PbrtParam>& attributes = {})
	{
		std::optional<std::string> type = quoted();
		if (!type.has_value()) {
			return std::nullopt;
		}
		PbrtEntity result;
		result.type = *type;
		result.loc = directive.loc;
		result.params = withAttributes(parameters(), attributes);
		if (m_failed) {
			return std::nullopt;
		}
		return result;
	}

	bool inWorld(const Token& directive)
	{
		if (!m_world) {
			fail(directive.loc, fmt::format("{} is only allowed after WorldBegin", directive.text));
		}
		return m_world;
	}

	bool beforeWorld(const Token& directive)
	{
		if (m_world) {
			fail(directive.loc, fmt::format("{} is only allowed before WorldBegin", directive.text));
		}
		return !m_world;
	}

	void include(const Token& directive, bool import)
	{
		std::optional<std::string> name = quoted();
		if (!name.has_value()) {
			return;
		}
		if (import && !m_world) {
			fail(directive.loc, "Import statement only allowed inside world definition block");
			return;
		}
		std::filesystem::path path = *name;
		if (path.is_relative()) {
			path = m_scene.directory / path;
		}
		std::optional<std::string> text = readFileText(path);
		if (!text.has_value()) {
			fail(directive.loc, fmt::format("{}: cannot read {}", directive.text, path.string()));
			return;
		}
		const uint32_t file = (uint32_t)m_scene.files.size();
		m_scene.files.push_back(path);
		if (import) {
			m_importSaved.push_back(m_state);
		}
		m_files.push_back(File { std::make_unique<Scanner>(std::move(*text), file), import, m_stack.size() });
	}

	void shape(const Token& directive)
	{
		std::optional<std::string> type = quoted();
		if (!type.has_value() || !inWorld(directive)) {
			return;
		}
		if (m_options.skipShapes.count(*type) != 0) {
			//its parameters still have to be read past
			parameters();
			auto [entry, inserted] = m_scene.skippedShapes.try_emplace(*type, 0, directive.loc);
			entry->second.first++;
			return;
		}
		PbrtShape shape;
		shape.type = *type;
		shape.loc = directive.loc;
		shape.params = withAttributes(parameters(), m_state.shapeAttributes);
		if (m_failed) {
			return;
		}
		shape.worldFromObject = m_state.ctm[0];
		shape.animated = m_state.ctm[0] != m_state.ctm[1];
		shape.reverseOrientation = m_state.reverseOrientation;
		shape.material = m_state.material;
		shape.insideMedium = m_state.insideMedium;
		shape.outsideMedium = m_state.outsideMedium;
		if (!m_state.areaLight.type.empty()) {
			if (m_object != nullptr) {
				warn(directive.loc, "area lights not supported with object instancing");
			} else {
				shape.areaLight = (int)m_scene.areaLights.size();
				m_scene.areaLights.push_back(m_state.areaLight);
			}
		}
		if (m_object != nullptr) {
			m_object->shapes.push_back(std::move(shape));
		} else {
			m_scene.shapes.push_back(std::move(shape));
		}
	}

	void directive(const Token& token)
	{
		m_lastLoc = token.loc;
		const std::string_view name = token.text;

		if (name == "AttributeBegin" || name == "TransformBegin") {
			if (name == "TransformBegin" && !m_warnedTransformBegin) {
				warn(token.loc, "TransformBegin/End are deprecated and should be replaced with AttributeBegin/End");
				m_warnedTransformBegin = true;
			}
			m_stack.push_back({ m_state, 'a', token.loc });
		} else if (name == "AttributeEnd" || name == "TransformEnd") {
			if (m_stack.empty()) {
				warn(token.loc, "unmatched AttributeEnd encountered; ignoring it");
				return;
			}
			if (m_stack.back().kind == 'o') {
				fail(token.loc, fmt::format("mismatched nesting: open ObjectBegin from {} at AttributeEnd", m_scene.where(m_stack.back().loc)));
				return;
			}
			m_state = std::move(m_stack.back().state);
			m_stack.pop_back();
		} else if (name == "Attribute") {
			std::optional<std::string> target = quoted();
			if (!target.has_value()) {
				return;
			}
			std::vector<PbrtParam> params = parameters();
			std::vector<PbrtParam>* attributes = nullptr;
			if (*target == "shape") {
				attributes = &m_state.shapeAttributes;
			} else if (*target == "light") {
				attributes = &m_state.lightAttributes;
			} else if (*target == "material") {
				attributes = &m_state.materialAttributes;
			} else if (*target == "medium") {
				attributes = &m_state.mediumAttributes;
			} else if (*target == "texture") {
				attributes = &m_state.textureAttributes;
			} else {
				fail(token.loc, fmt::format("unknown attribute target \"{}\"", *target));
				return;
			}
			for (PbrtParam& p : params) {
				attributes->push_back(std::move(p));
			}
		} else if (name == "ActiveTransform") {
			std::optional<Token> which = required();
			if (!which.has_value()) {
				return;
			}
			if (which->text == "All") {
				m_state.activeTransforms = ALL_TRANSFORMS;
			} else if (which->text == "EndTime") {
				m_state.activeTransforms = END_TRANSFORM;
			} else if (which->text == "StartTime") {
				m_state.activeTransforms = START_TRANSFORM;
			} else {
				fail(which->loc, fmt::format("unknown ActiveTransform \"{}\"", which->text));
			}
		} else if (name == "AreaLightSource") {
			if (std::optional<PbrtEntity> light = entity(token, m_state.lightAttributes); light.has_value() && inWorld(token)) {
				m_state.areaLight = std::move(*light);
			}
		} else if (name == "Accelerator") {
			if (std::optional<PbrtEntity> e = entity(token); e.has_value() && beforeWorld(token)) {
				m_scene.accelerator = std::move(*e);
			}
		} else if (name == "ConcatTransform") {
			double m[16];
			if (bracketedNumbers(token, m, 16)) {
				const glm::dmat4 matrix = glm::make_mat4(m);
				m_state.forActiveTransforms([&](const glm::dmat4& t) { return t * matrix; });
			}
		} else if (name == "CoordinateSystem") {
			if (std::optional<std::string> n = quoted()) {
				m_coordinateSystems[*n] = { m_state.ctm[0], m_state.ctm[1] };
			}
		} else if (name == "CoordSysTransform") {
			if (std::optional<std::string> n = quoted()) {
				const auto found = m_coordinateSystems.find(*n);
				if (found == m_coordinateSystems.end()) {
					warn(token.loc, fmt::format("couldn't find named coordinate system \"{}\"", *n));
				} else {
					m_state.ctm[0] = found->second[0];
					m_state.ctm[1] = found->second[1];
				}
			}
		} else if (name == "ColorSpace") {
			if (std::optional<std::string> n = quoted()) {
				m_state.colorSpace = *n;
				m_scene.colorSpace = *n;
			}
		} else if (name == "Camera") {
			std::optional<PbrtEntity> e = entity(token);
			if (!e.has_value() || !beforeWorld(token)) {
				return;
			}
			PbrtCamera camera;
			static_cast<PbrtEntity&>(camera) = std::move(*e);
			camera.cameraFromWorld = m_state.ctm[0];
			if (m_state.ctm[0] != m_state.ctm[1]) {
				warn(token.loc, "the camera's transform is animated; the shutter-open transform is used");
			}
			m_coordinateSystems["camera"] = { glm::inverse(m_state.ctm[0]), glm::inverse(m_state.ctm[1]) };
			m_scene.camera = std::move(camera);
		} else if (name == "Film") {
			if (std::optional<PbrtEntity> e = entity(token); e.has_value() && beforeWorld(token)) {
				m_scene.film = std::move(*e);
			}
		} else if (name == "Integrator") {
			if (std::optional<PbrtEntity> e = entity(token); e.has_value() && beforeWorld(token)) {
				m_scene.integrator = std::move(*e);
			}
		} else if (name == "Include") {
			include(token, false);
		} else if (name == "Import") {
			include(token, true);
		} else if (name == "Identity") {
			m_state.forActiveTransforms([](const glm::dmat4&) { return glm::dmat4(1.0); });
		} else if (name == "LightSource") {
			std::optional<PbrtEntity> e = entity(token, m_state.lightAttributes);
			if (!e.has_value() || !inWorld(token)) {
				return;
			}
			PbrtLight light;
			static_cast<PbrtEntity&>(light) = std::move(*e);
			light.worldFromLight = m_state.ctm[0];
			light.medium = m_state.outsideMedium;
			m_scene.lights.push_back(std::move(light));
		} else if (name == "LookAt") {
			double v[9];
			for (double& value : v) {
				std::optional<double> n = number();
				if (!n.has_value()) {
					return;
				}
				value = *n;
			}
			bool degenerate = false;
			const glm::dmat4 lookAt = pbrtLookAt({ v[0], v[1], v[2] }, { v[3], v[4], v[5] }, { v[6], v[7], v[8] }, degenerate);
			if (degenerate) {
				fail(token.loc, "LookAt: \"up\" vector and viewing direction are pointing in the same direction");
				return;
			}
			m_state.forActiveTransforms([&](const glm::dmat4& t) { return t * lookAt; });
		} else if (name == "MakeNamedMaterial") {
			std::optional<std::string> n = quoted();
			if (!n.has_value()) {
				return;
			}
			PbrtEntity material;
			material.loc = token.loc;
			material.params = withAttributes(parameters(), m_state.materialAttributes);
			if (m_failed || !inWorld(token)) {
				return;
			}
			material.type = material.params.getString("type", "");
			if (m_scene.namedMaterials.count(*n) != 0) {
				fail(token.loc, fmt::format("{}: named material redefined", *n));
				return;
			}
			m_scene.namedMaterials[*n] = std::move(material);
		} else if (name == "MakeNamedMedium") {
			std::optional<std::string> n = quoted();
			if (!n.has_value()) {
				return;
			}
			PbrtMedium medium;
			medium.loc = token.loc;
			medium.params = withAttributes(parameters(), m_state.mediumAttributes);
			if (m_failed) {
				return;
			}
			medium.type = medium.params.getString("type", "");
			medium.worldFromMedium = m_state.ctm[0];
			if (m_scene.media.count(*n) != 0) {
				fail(token.loc, fmt::format("named medium \"{}\" redefined", *n));
				return;
			}
			m_scene.media[*n] = std::move(medium);
		} else if (name == "Material") {
			std::optional<PbrtEntity> e = entity(token, m_state.materialAttributes);
			if (!e.has_value() || !inWorld(token)) {
				return;
			}
			m_state.material = { (int)m_scene.materials.size(), {} };
			m_scene.materials.push_back(std::move(*e));
		} else if (name == "MediumInterface") {
			std::optional<std::string> inside = quoted();
			if (!inside.has_value()) {
				return;
			}
			std::string outside = *inside;
			//the second name is optional
			if (std::optional<Token> second = nextToken(false)) {
				if (isQuoted(second->text)) {
					outside = std::string(dequote(second->text));
				} else {
					m_unget = second;
				}
			}
			m_state.insideMedium = *inside;
			m_state.outsideMedium = outside;
		} else if (name == "NamedMaterial") {
			if (std::optional<std::string> n = quoted(); n.has_value() && inWorld(token)) {
				m_state.material = { -1, *n };
			}
		} else if (name == "ObjectBegin") {
			std::optional<std::string> n = quoted();
			if (!n.has_value() || !inWorld(token)) {
				return;
			}
			m_stack.push_back({ m_state, 'o', token.loc });
			if (m_object != nullptr) {
				fail(token.loc, "ObjectBegin called inside of instance definition");
				return;
			}
			if (m_scene.objects.count(*n) != 0) {
				fail(token.loc, fmt::format("{}: trying to redefine an object instance", *n));
				return;
			}
			m_object = &m_scene.objects[*n];
			m_object->name = *n;
			m_object->loc = token.loc;
		} else if (name == "ObjectEnd") {
			if (!inWorld(token)) {
				return;
			}
			if (m_object == nullptr) {
				fail(token.loc, "ObjectEnd called outside of instance definition");
				return;
			}
			if (m_stack.empty() || m_stack.back().kind != 'o') {
				fail(token.loc, "mismatched nesting: open AttributeBegin at ObjectEnd");
				return;
			}
			m_state = std::move(m_stack.back().state);
			m_stack.pop_back();
			m_object = nullptr;
		} else if (name == "ObjectInstance") {
			std::optional<std::string> n = quoted();
			if (!n.has_value() || !inWorld(token)) {
				return;
			}
			if (m_object != nullptr) {
				fail(token.loc, "ObjectInstance can't be called inside instance definition");
				return;
			}
			m_scene.instances.push_back({ *n, m_state.ctm[0], m_state.ctm[0] != m_state.ctm[1], token.loc });
		} else if (name == "Option") {
			std::optional<std::string> n = quoted();
			std::optional<Token> value = n.has_value() ? required() : std::nullopt;
			if (value.has_value()) {
				m_scene.options.emplace_back(*n, std::string(isQuoted(value->text) ? dequote(value->text) : value->text));
			}
		} else if (name == "PixelFilter") {
			if (std::optional<PbrtEntity> e = entity(token); e.has_value() && beforeWorld(token)) {
				m_scene.filter = std::move(*e);
			}
		} else if (name == "ReverseOrientation") {
			if (inWorld(token)) {
				m_state.reverseOrientation = !m_state.reverseOrientation;
			}
		} else if (name == "Rotate") {
			double v[4];
			for (double& value : v) {
				std::optional<double> n = number();
				if (!n.has_value()) {
					return;
				}
				value = *n;
			}
			const glm::dvec3 axis(v[1], v[2], v[3]);
			if (glm::length(axis) == 0.0) {
				fail(token.loc, "Rotate: zero-length axis");
				return;
			}
			const glm::dmat4 rotation = glm::rotate(glm::dmat4(1.0), glm::radians(v[0]), axis);
			m_state.forActiveTransforms([&](const glm::dmat4& t) { return t * rotation; });
		} else if (name == "Shape") {
			shape(token);
		} else if (name == "Sampler") {
			if (std::optional<PbrtEntity> e = entity(token); e.has_value() && beforeWorld(token)) {
				m_scene.sampler = std::move(*e);
			}
		} else if (name == "Scale") {
			double v[3];
			for (double& value : v) {
				std::optional<double> n = number();
				if (!n.has_value()) {
					return;
				}
				value = *n;
			}
			const glm::dmat4 scale = glm::scale(glm::dmat4(1.0), glm::dvec3(v[0], v[1], v[2]));
			m_state.forActiveTransforms([&](const glm::dmat4& t) { return t * scale; });
		} else if (name == "Transform") {
			double m[16];
			if (bracketedNumbers(token, m, 16)) {
				const glm::dmat4 matrix = glm::make_mat4(m);
				m_state.forActiveTransforms([&](const glm::dmat4&) { return matrix; });
			}
		} else if (name == "Translate") {
			double v[3];
			for (double& value : v) {
				std::optional<double> n = number();
				if (!n.has_value()) {
					return;
				}
				value = *n;
			}
			const glm::dmat4 translation = glm::translate(glm::dmat4(1.0), glm::dvec3(v[0], v[1], v[2]));
			m_state.forActiveTransforms([&](const glm::dmat4& t) { return t * translation; });
		} else if (name == "TransformTimes") {
			std::optional<double> start = number();
			std::optional<double> end = start.has_value() ? number() : std::nullopt;
			if (end.has_value()) {
				m_scene.transformStartTime = *start;
				m_scene.transformEndTime = *end;
			}
		} else if (name == "Texture") {
			std::optional<std::string> textureName = quoted();
			std::optional<std::string> type = textureName.has_value() ? quoted() : std::nullopt;
			std::optional<std::string> className = type.has_value() ? quoted() : std::nullopt;
			if (!className.has_value()) {
				return;
			}
			PbrtTexture texture;
			texture.name = *textureName;
			texture.className = *className;
			texture.loc = token.loc;
			texture.params = withAttributes(parameters(), m_state.textureAttributes);
			texture.worldFromTexture = m_state.ctm[0];
			if (m_failed || !inWorld(token)) {
				return;
			}
			if (*type != "float" && *type != "spectrum") {
				fail(token.loc, fmt::format("{}: texture type unknown. Must be \"float\" or \"spectrum\"", *type));
				return;
			}
			texture.spectrum = *type == "spectrum";
			std::map<std::string, PbrtTexture>& textures = texture.spectrum ? m_scene.spectrumTextures : m_scene.floatTextures;
			if (textures.count(texture.name) != 0) {
				fail(token.loc, fmt::format("redefining texture \"{}\"", texture.name));
				return;
			}
			textures[texture.name] = std::move(texture);
		} else if (name == "WorldBegin") {
			if (!beforeWorld(token)) {
				return;
			}
			m_world = true;
			m_state.ctm[0] = m_state.ctm[1] = glm::dmat4(1.0);
			m_state.activeTransforms = ALL_TRANSFORMS;
			m_coordinateSystems["world"] = { m_state.ctm[0], m_state.ctm[1] };
		} else if (name == "WorldEnd") {
			//pbrt-v3's closing directive; pbrt-v4 only accepts it when reformatting a file
		} else {
			fail(token.loc, fmt::format("unknown directive: {}", name));
		}
	}

	void finish()
	{
		if (!m_world) {
			fail(m_lastLoc, "end of files before \"WorldBegin\"");
			return;
		}
		for (const StackEntry& entry : m_stack) {
			warn(entry.loc, entry.kind == 'o' ? "missing ObjectEnd" : "missing end to AttributeBegin");
		}
	}

	struct StackEntry {
		GraphicsState state;
		// 'a' AttributeBegin, 'o' ObjectBegin
		char kind;
		PbrtLoc loc;
	};

	PbrtScene& m_scene;
	const PbrtParseOptions& m_options;
	std::vector<File> m_files;
	std::vector<std::unique_ptr<Scanner>> m_finished;
	std::optional<Token> m_unget;
	GraphicsState m_state;
	std::vector<StackEntry> m_stack;
	std::vector<GraphicsState> m_importSaved;
	std::vector<size_t> m_importDepths;
	int m_pendingRestores { 0 };
	std::map<std::string, std::array<glm::dmat4, 2>> m_coordinateSystems;
	PbrtObject* m_object { nullptr };
	bool m_world { false };
	bool m_warnedTransformBegin { false };
	bool m_failed { false };
	std::string m_error;
	PbrtLoc m_lastLoc;
};

}

//---------------------------------------------------------------- parameters

const PbrtParam* PbrtParams::find(std::string_view name) const
{
	for (const PbrtParam& p : list) {
		if (p.name == name) {
			p.used = true;
			return &p;
		}
	}
	return nullptr;
}

double PbrtParams::getFloat(std::string_view name, double fallback) const
{
	const PbrtParam* p = find(name);
	return p != nullptr && !p->numbers.empty() ? p->numbers[0] : fallback;
}

int PbrtParams::getInt(std::string_view name, int fallback) const
{
	const PbrtParam* p = find(name);
	return p != nullptr && !p->numbers.empty() ? (int)p->numbers[0] : fallback;
}

bool PbrtParams::getBool(std::string_view name, bool fallback) const
{
	const PbrtParam* p = find(name);
	return p != nullptr && !p->bools.empty() ? (bool)p->bools[0] : fallback;
}

std::string PbrtParams::getString(std::string_view name, std::string_view fallback) const
{
	const PbrtParam* p = find(name);
	return p != nullptr && !p->strings.empty() ? p->strings[0] : std::string(fallback);
}

std::optional<glm::dvec3> PbrtParams::getVector3(std::string_view name) const
{
	const PbrtParam* p = find(name);
	if (p == nullptr || p->numbers.size() < 3) {
		return std::nullopt;
	}
	return glm::dvec3(p->numbers[0], p->numbers[1], p->numbers[2]);
}

std::vector<double> PbrtParams::getNumbers(std::string_view name) const
{
	const PbrtParam* p = find(name);
	return p != nullptr ? p->numbers : std::vector<double> {};
}

std::string PbrtParams::getTexture(std::string_view name) const
{
	const PbrtParam* p = find(name);
	return p != nullptr && p->type == "texture" && !p->strings.empty() ? p->strings[0] : std::string();
}

std::vector<const PbrtParam*> PbrtParams::unused() const
{
	std::vector<const PbrtParam*> result;
	for (const PbrtParam& p : list) {
		if (!p.used) {
			result.push_back(&p);
		}
	}
	return result;
}

std::string PbrtScene::where(const PbrtLoc& loc) const
{
	const std::string file = loc.file < files.size() ? files[loc.file].filename().string() : std::string("<string>");
	//line 0 is a message about the whole file, not a place in it
	return loc.line > 0 ? fmt::format("{}:{}", file, loc.line) : file;
}

//---------------------------------------------------------------- entry points

bool parsePbrtFile(const std::filesystem::path& path, PbrtScene& out, std::string* error, const PbrtParseOptions& options)
{
	std::optional<std::string> text = readFileText(path);
	if (!text.has_value()) {
		if (error != nullptr) {
			*error = fmt::format("cannot read {}", path.string());
		}
		return false;
	}
	out = PbrtScene {};
	out.files.push_back(path);
	out.directory = path.parent_path();
	Parser parser(out, options);
	std::string message;
	const bool ok = parser.run(std::make_unique<Scanner>(std::move(*text), 0), message);
	if (!ok && error != nullptr) {
		*error = message;
	}
	return ok;
}

bool parsePbrtString(std::string text, PbrtScene& out, std::string* error, const std::filesystem::path& directory, const PbrtParseOptions& options)
{
	out = PbrtScene {};
	out.files.push_back("<string>");
	out.directory = directory;
	Parser parser(out, options);
	std::string message;
	const bool ok = parser.run(std::make_unique<Scanner>(std::move(text), 0), message);
	if (!ok && error != nullptr) {
		*error = message;
	}
	return ok;
}

bool scanPbrtTokens(std::string text, std::vector<std::string>& tokens, std::string* error)
{
	Scanner scanner(std::move(text), 0);
	tokens.clear();
	while (true) {
		std::string message;
		std::optional<Token> token = scanner.next(message);
		if (!message.empty()) {
			if (error != nullptr) {
				*error = message;
			}
			return false;
		}
		if (!token.has_value()) {
			return true;
		}
		tokens.emplace_back(token->text);
	}
}
