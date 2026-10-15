// wget-curl 内部单元白盒测试：文件名推导 / CLI 解析 / 配置文件解析
// （配置测试重定向 USERPROFILE 到临时目录，绝不触碰真实用户目录）。

#include "CppUTest/TestHarness.h"

#include "cli.hpp"
#include "config.hpp"
#include "filename.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

std::filesystem::path tmp_root() {
    // 写入系统临时目录（而不是源码树）：跑完测试后源码目录保持干净
    std::error_code ec0;
    static std::filesystem::path base =
        std::filesystem::temp_directory_path(ec0) / "pjfetch_tests" / "wget_units";
    std::error_code ec;
    std::filesystem::create_directories(base, ec);
    return base;
}

void set_env(const char* name, const char* value) {
#ifdef _WIN32
    _putenv_s(name, value);
#else
    setenv(name, value, 1);
#endif
}

std::vector<std::string> argv_of(std::initializer_list<const char*> items) {
    return std::vector<std::string>(items.begin(), items.end());
}

} // namespace

// 文件名推导
TEST_GROUP(Filename){};

TEST(Filename, url_last_segment_is_used) {
    STRCMP_EQUAL("file.zip", pjf_cli::filename_from_url("http://h/a/b/file.zip").c_str());
}

TEST(Filename, url_query_string_is_stripped) {
    STRCMP_EQUAL("file.bin", pjf_cli::filename_from_url("http://h/file.bin?token=abc&x=1").c_str());
}

TEST(Filename, url_fragment_is_stripped) {
    STRCMP_EQUAL("file.bin", pjf_cli::filename_from_url("http://h/file.bin#section").c_str());
}

TEST(Filename, url_without_path_yields_empty) {
    STRCMP_EQUAL("", pjf_cli::filename_from_url("http://h").c_str());
    STRCMP_EQUAL("", pjf_cli::filename_from_url("http://h/").c_str());
}

TEST(Filename, url_percent_encoding_is_decoded) {
    STRCMP_EQUAL("a b.txt", pjf_cli::filename_from_url("http://h/a%20b.txt").c_str());
}

TEST(Filename, sanitize_replaces_windows_illegal_chars) {
    STRCMP_EQUAL("a_b_c_d_e_f_g_h_i", pjf_cli::sanitize_filename("a<b>c:d\"e/f\\g|h?i").c_str());
}

TEST(Filename, sanitize_replaces_control_characters) {
    // 逐字符替换，不折叠
    STRCMP_EQUAL("a__b", pjf_cli::sanitize_filename(std::string("a\x01\x02") + "b").c_str());
    STRCMP_EQUAL("a_b", pjf_cli::sanitize_filename(std::string("a\x01") + "b").c_str());
}

TEST(Filename, sanitize_strips_trailing_dot_and_space) {
    STRCMP_EQUAL("name", pjf_cli::sanitize_filename("name...").c_str());
    STRCMP_EQUAL("name", pjf_cli::sanitize_filename("name   ").c_str());
}

TEST(Filename, sanitize_suffixes_reserved_device_names) {
    STRCMP_EQUAL("CON_", pjf_cli::sanitize_filename("CON").c_str());
    STRCMP_EQUAL("nul_", pjf_cli::sanitize_filename("nul").c_str());
    STRCMP_EQUAL("COM1_", pjf_cli::sanitize_filename("COM1").c_str());
    STRCMP_EQUAL("console.txt", pjf_cli::sanitize_filename("console.txt").c_str());   // 非保留名
}

TEST(Filename, sanitize_limits_length_keeping_extension) {
    // 截断到 <=200 字节并保留末尾扩展名
    std::string long_name(400, 'x');
    std::string got = pjf_cli::sanitize_filename(long_name + ".tar.gz");
    CHECK(got.size() <= 200);
    STRCMP_EQUAL(".gz", got.substr(got.size() - 3).c_str());

    std::string got2 = pjf_cli::sanitize_filename(std::string(400, 'y') + ".bin");
    CHECK(got2.size() <= 200);
    STRCMP_EQUAL(".bin", got2.substr(got2.size() - 4).c_str());

    STRCMP_EQUAL("short.tar.gz", pjf_cli::sanitize_filename("short.tar.gz").c_str());
}

TEST(Filename, sanitize_empty_input_stays_empty) {
    STRCMP_EQUAL("", pjf_cli::sanitize_filename("").c_str());
}

TEST(Filename, content_disposition_plain_filename) {
    STRCMP_EQUAL("report.pdf",
                 pjf_cli::filename_from_content_disposition("attachment; filename=\"report.pdf\"").c_str());
}

TEST(Filename, content_disposition_unquoted_filename) {
    STRCMP_EQUAL("report.pdf",
                 pjf_cli::filename_from_content_disposition("attachment; filename=report.pdf").c_str());
}

TEST(Filename, content_disposition_star_form_takes_precedence) {
    STRCMP_EQUAL("dürich.bin",
                 pjf_cli::filename_from_content_disposition(
                     "attachment; filename=\"plain.bin\"; filename*=UTF-8''d%C3%BCrich.bin").c_str());
}

TEST(Filename, content_disposition_basename_only) {
    STRCMP_EQUAL("f.bin",
                 pjf_cli::filename_from_content_disposition("attachment; filename=\"/etc/passwd/f.bin\"").c_str());
}

TEST(Filename, unique_path_returns_same_path_when_free) {
    auto p = tmp_root() / "unique_probe.bin";
    std::error_code ec;
    std::filesystem::remove(p, ec);
    STRCMP_EQUAL(p.string().c_str(), pjf_cli::unique_path(p).string().c_str());
}

TEST(Filename, unique_path_avoids_existing_file) {
    auto p = tmp_root() / "collide.bin";
    { std::ofstream f(p); f << "x"; }
    auto got = pjf_cli::unique_path(p);
    CHECK(got != p);
    CHECK_FALSE(std::filesystem::exists(got));
}

// CLI 解析
TEST_GROUP(CliParse) {
    pjf_cli::options defaults;
    pjf_cli::options out;
    std::string err, text;

    pjf_cli::parse_status run(std::initializer_list<const char*> items) {
        err.clear();
        text.clear();
        out = pjf_cli::options{};
        return pjf_cli::parse(argv_of(items), defaults, out, err, text);
    }
};

TEST(CliParse, url_only_uses_defaults) {
    LONGS_EQUAL((int)pjf_cli::parse_status::ok, (int)run({"http://h/f.bin"}));
    STRCMP_EQUAL("http://h/f.bin", out.url.c_str());
    LONGS_EQUAL(8, out.para_jobs);
    CHECK(out.resume);
    CHECK(out.verify_ssl);
}

TEST(CliParse, para_jobs_as_first_argument_sets_segment_count) {
    LONGS_EQUAL((int)pjf_cli::parse_status::ok, (int)run({"para-jobs=16", "http://h/f.bin"}));
    LONGS_EQUAL(16, out.para_jobs);
    STRCMP_EQUAL("http://h/f.bin", out.url.c_str());
}

TEST(CliParse, para_jobs_clamps_to_valid_range) {
    LONGS_EQUAL((int)pjf_cli::parse_status::ok, (int)run({"para-jobs=999", "http://h/f.bin"}));
    LONGS_EQUAL(32, out.para_jobs);
    LONGS_EQUAL((int)pjf_cli::parse_status::ok, (int)run({"para-jobs=0", "http://h/f.bin"}));
    LONGS_EQUAL(1, out.para_jobs);
}

TEST(CliParse, para_jobs_in_non_first_position_is_error) {
    LONGS_EQUAL((int)pjf_cli::parse_status::error, (int)run({"http://h/f.bin", "para-jobs=4"}));
    CHECK(err.size() > 0);
}

TEST(CliParse, malformed_para_jobs_is_error) {
    LONGS_EQUAL((int)pjf_cli::parse_status::error, (int)run({"para-jobs=abc", "http://h/f.bin"}));
    LONGS_EQUAL((int)pjf_cli::parse_status::error, (int)run({"para-jobs", "http://h/f.bin"}));
}

TEST(CliParse, long_option_with_equals_and_separate_value) {
    LONGS_EQUAL((int)pjf_cli::parse_status::ok, (int)run({"--retry=3", "--user-agent", "UA/1", "http://h/f.bin"}));
    LONGS_EQUAL(3, out.retry);
    STRCMP_EQUAL("UA/1", out.user_agent.c_str());
}

TEST(CliParse, output_and_directory_options) {
    LONGS_EQUAL((int)pjf_cli::parse_status::ok, (int)run({"-O", "out.bin", "-P", "dir", "http://h/f.bin"}));
    STRCMP_EQUAL("out.bin", out.output.c_str());
    STRCMP_EQUAL("dir", out.dir.c_str());
}

TEST(CliParse, clustered_short_flags) {
    LONGS_EQUAL((int)pjf_cli::parse_status::ok, (int)run({"-qv", "http://h/f.bin"}));
    CHECK(out.quiet);
    CHECK(out.verbose);
}

TEST(CliParse, no_resume_disables_resume) {
    LONGS_EQUAL((int)pjf_cli::parse_status::ok, (int)run({"--no-resume", "http://h/f.bin"}));
    CHECK_FALSE(out.resume);
}

TEST(CliParse, no_check_certificate_disables_ssl_verification) {
    LONGS_EQUAL((int)pjf_cli::parse_status::ok, (int)run({"--no-check-certificate", "http://h/f.bin"}));
    CHECK_FALSE(out.verify_ssl);
}

TEST(CliParse, repeated_headers_are_joined_with_crlf) {
    LONGS_EQUAL((int)pjf_cli::parse_status::ok,
                (int)run({"--header", "A: 1", "--header", "B: 2", "http://h/f.bin"}));
    STRCMP_EQUAL("A: 1\r\nB: 2", out.headers.c_str());
}

TEST(CliParse, unknown_option_is_error) {
    LONGS_EQUAL((int)pjf_cli::parse_status::error, (int)run({"--nonsense", "http://h/f.bin"}));
    CHECK(err.size() > 0);
}

TEST(CliParse, missing_url_is_error) {
    LONGS_EQUAL((int)pjf_cli::parse_status::error, (int)run({"-O", "out.bin"}));
}

TEST(CliParse, two_urls_are_error) {
    LONGS_EQUAL((int)pjf_cli::parse_status::error, (int)run({"http://a/f", "http://b/g"}));
}

TEST(CliParse, non_numeric_value_is_error) {
    LONGS_EQUAL((int)pjf_cli::parse_status::error, (int)run({"--retry", "many", "http://h/f.bin"}));
}

TEST(CliParse, help_and_version_are_reported) {
    LONGS_EQUAL((int)pjf_cli::parse_status::help, (int)run({"-h"}));
    CHECK(text.size() > 0);
    LONGS_EQUAL((int)pjf_cli::parse_status::version, (int)run({"--version"}));
    CHECK(text.size() > 0);
}

TEST(CliParse, config_defaults_are_overridden_by_explicit_cli) {
    pjf_cli::options d;
    d.para_jobs = 4;
    d.retry = 9;
    err.clear(); text.clear();
    pjf_cli::options o2;
    LONGS_EQUAL((int)pjf_cli::parse_status::ok,
                (int)pjf_cli::parse({"http://h/f.bin"}, d, o2, err, text));
    LONGS_EQUAL(4, o2.para_jobs);        // 来自 defaults
    LONGS_EQUAL(9, o2.retry);

    LONGS_EQUAL((int)pjf_cli::parse_status::ok,
                (int)pjf_cli::parse({"para-jobs=2", "--retry", "1", "http://h/f.bin"}, d, o2, err, text));
    LONGS_EQUAL(2, o2.para_jobs);        // CLI 覆盖
    LONGS_EQUAL(1, o2.retry);
}

TEST(CliParse, apply_config_defaults_maps_known_keys) {
    std::map<std::string, std::string> cfg{
        {"para-jobs", "12"}, {"retry", "2"}, {"user-agent", "CFG/1"}, {"resume", "off"}};
    pjf_cli::options o;
    pjf_cli::apply_config_defaults(cfg, o);
    LONGS_EQUAL(12, o.para_jobs);
    LONGS_EQUAL(2, o.retry);
    STRCMP_EQUAL("CFG/1", o.user_agent.c_str());
    CHECK_FALSE(o.resume);
}

TEST(CliParse, apply_config_defaults_reports_unknown_keys) {
    std::map<std::string, std::string> cfg{{"totally-unknown", "1"}};
    pjf_cli::options o;
    CHECK(pjf_cli::apply_config_defaults(cfg, o) >= 1);
}

// 配置文件
TEST_GROUP(ConfigFile) {
    std::filesystem::path home, cfgdir;
    std::string saved_profile;

    void setup() override {
        const char* prev = std::getenv("USERPROFILE");
        saved_profile = prev ? prev : "";
        home = tmp_root() / "fakehome";
        cfgdir = home / ".pjfetch";
        std::error_code ec;
        std::filesystem::remove_all(home, ec);
        std::filesystem::create_directories(cfgdir, ec);
        set_env("USERPROFILE", home.string().c_str());
#if !defined(_WIN32)
        set_env("HOME", home.string().c_str());
#endif
    }
    void teardown() override {
        set_env("USERPROFILE", saved_profile.c_str());
        std::error_code ec;
        std::filesystem::remove_all(home, ec);
    }
    void write_cfg(const std::string& body) {
        std::ofstream f(cfgdir / "pj-cfg.cnf", std::ios::binary | std::ios::trunc);
        f << body;
    }
};

TEST(ConfigFile, missing_file_yields_empty_map) {
    CHECK(pjf_cli::load_config().empty());
}

TEST(ConfigFile, parses_global_section_keys) {
    write_cfg("[global]\npara-jobs = 16\nretry=3\nuser-agent = MyUA/2.0\n");
    auto cfg = pjf_cli::load_config();
    LONGS_EQUAL(3, (long)cfg.size());
    STRCMP_EQUAL("16", cfg["para-jobs"].c_str());
    STRCMP_EQUAL("3", cfg["retry"].c_str());
    STRCMP_EQUAL("MyUA/2.0", cfg["user-agent"].c_str());
}

TEST(ConfigFile, ignores_other_sections) {
    write_cfg("[global]\nretry=3\n[other]\nretry=99\n");
    auto cfg = pjf_cli::load_config();
    LONGS_EQUAL(1, (long)cfg.size());
    STRCMP_EQUAL("3", cfg["retry"].c_str());
}

TEST(ConfigFile, skips_comments_and_blank_lines) {
    write_cfg("# comment\n; also comment\n\n[global]\n\n#x\nretry = 7\n");
    auto cfg = pjf_cli::load_config();
    LONGS_EQUAL(1, (long)cfg.size());
    STRCMP_EQUAL("7", cfg["retry"].c_str());
}

TEST(ConfigFile, keys_are_case_insensitive_lowercased) {
    write_cfg("[GLOBAL]\nRetry = 5\n");
    auto cfg = pjf_cli::load_config();
    CHECK(cfg.count("retry") == 1);
    STRCMP_EQUAL("5", cfg["retry"].c_str());
}

TEST(ConfigFile, malformed_lines_are_skipped_not_fatal) {
    write_cfg("[global]\nthis line has no equals\nretry=4\n");
    auto cfg = pjf_cli::load_config();
    LONGS_EQUAL(1, (long)cfg.size());
    STRCMP_EQUAL("4", cfg["retry"].c_str());
}

TEST(ConfigFile, keys_before_any_section_are_accepted) {
    write_cfg("retry=6\n[global]\npara-jobs=2\n");
    auto cfg = pjf_cli::load_config();
    LONGS_EQUAL(2, (long)cfg.size());
    STRCMP_EQUAL("6", cfg["retry"].c_str());
}
