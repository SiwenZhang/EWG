// This program reads all the issues in the issues directory passed as the first command line argument.
// If all documents are successfully parsed, it will generate the standard EWG Issues List  documents
// for an ISO SC22 WG21 mailing.

// Based on code originally donated by Howard Hinnant
// Since modified by Alisdair Meredith

// Note that this program requires a reasonably conformant C++0x compiler, supporting at least:
//    auto
//    lambda expressions
//    brace-initialization
//    range-based for loops
//    raw string literals
//    constexpr
//    new function syntax (late specified return type)
//    noexcept
// Its coding style assumes a standard library optimized with move-semantics
// The only known compiler to support all of this today is the experimental gcc trunk (4.6)

// TODO
// .  Better handling of TR "sections", and grouping of issues in "Clause X"
// .  Sort the Revision comments in the same order as the 'Status' reports, rather than alphabetically
// .  Lots of tidy and cleanup after merging the revision-generating tool
// .  Refactor more common text
// .  Split 'format' function and usage to that the issues vector can pass by const-ref in the common cases
// .  Document the purpose amd contract on each function
// Waiting on external fix for preserving file-dates
// .  sort-by-last-modified-date should offer some filter or separation to see only the issues modified since  the last meeting


// standard headers
#include <sstream>
#include <iostream>
#include <fstream>
#include <string>
#include <vector>
#include <map>
#include <set>
#include <iterator>
#include <stdexcept>
#include <algorithm>
#include <cctype>
#include <ctime>
#include <cassert>

// platform headers - requires a Posix compatible platform
#include <sys/types.h>
#include <sys/stat.h>
#include <dirent.h>
#include <unistd.h>

// solution specific headers
#include "date.h"

namespace greg = gregorian;


// This should be part of <string> in 0x lib
// Should also be more efficient than using ostringstream!
auto itos(int i) -> std::string {
   std::ostringstream t;
   t << i;
   return t.str();
};


// Generic utilities that are useful and do not rely on context or types from our domain (issue-list processing)
// =============================================================================================================

auto parse_month(std::string const & m) -> greg::month {
   // This could be turned into an efficient map lookup with a suitable indexed container
   return (m == "Jan") ? greg::jan
        : (m == "Feb") ? greg::feb
        : (m == "Mar") ? greg::mar
        : (m == "Apr") ? greg::apr
        : (m == "May") ? greg::may
        : (m == "Jun") ? greg::jun
        : (m == "Jul") ? greg::jul
        : (m == "Aug") ? greg::aug
        : (m == "Sep") ? greg::sep
        : (m == "Oct") ? greg::oct
        : (m == "Nov") ? greg::nov
        : (m == "Dec") ? greg::dec
        : throw std::runtime_error{"unknown month"};
}

auto parse_date(std::istream & temp) -> greg::date {
   int d;
   temp >> d;
   if (temp.fail()) {
      throw std::runtime_error{"date format error"};
   }

   std::string month;
   temp >> month;

   auto m = parse_month(month);
   int y{ 0 };
   temp >> y;
   return m/greg::day{d}/y;
}


void print_date(std::ostream & out, greg::date const & mod_date ) {
   out << mod_date.year() << '-';
   if (mod_date.month() < 10) { out << '0'; }
   out << mod_date.month() << '-';
   if (mod_date.day() < 10) { out << '0'; }
   out << mod_date.day();
}


auto make_date(std::tm const & mod) -> greg::date {
   return greg::year((unsigned short)(mod.tm_year+1900)) / (mod.tm_mon+1) / mod.tm_mday;
}


auto report_date_file_last_modified(std::string const & filename) -> greg::date {
   struct stat buf;
   if (stat(filename.c_str(), &buf) == -1) {
      throw std::runtime_error{"call to stat failed"};
   }

   return make_date(*std::localtime(&buf.st_mtime));
}


auto format_time(std::string const & format, std::tm const & t) -> std::string {
   std::string s;
   std::size_t maxsize{format.size() + 256};
  //for (std::size_t maxsize = format.size() + 64; s.size() == 0 ; maxsize += 64)
  //{
      char * buf{new char[maxsize]};
      std::size_t size{std::strftime( buf, maxsize, format.c_str(), &t ) };
      if(size > 0) {
         s += buf;
      }
      delete[] buf;
 // }
   return s;
}

auto utc_timestamp() -> std::tm const & {
   static std::time_t t{ std::time(nullptr) };
   static std::tm utc = *std::gmtime(&t);
   return utc;
}


template<typename Container>
void print_list(std::ostream & out, Container const & source, char const * separator ) {
   char const * sep{""};
   for( auto const & x : source ) {
      out << sep << x;
      sep = separator;
   }
}


auto read_file_into_string(std::string const & filename) -> std::string {
   std::ifstream infile{filename.c_str()};
   if (!infile.is_open()) {
      throw std::runtime_error{"Unable to open file " + filename};
   }

   std::istreambuf_iterator<char> first{infile}, last{};
   return std::string {first, last};
}

// Issue-list specific functionality for the rest of this file
// ===========================================================

std::string build_timestamp(
  format_time("<p>Revised %Y-%m-%d at %H:%m:%S UTC</p>\n", utc_timestamp()));

// Functions to "normalize" a status string
auto remove_pending(std::string stat) -> std::string {
   typedef std::string::size_type size_type;
   if(0 == stat.find("Pending")) {
      stat.erase(size_type{0}, size_type{8});
   }
   return stat;
}

auto remove_tentatively(std::string stat) -> std::string {
   typedef std::string::size_type size_type;
   if(0 == stat.find("Tentatively")) {
      stat.erase(size_type{0}, size_type{12});
   }
   return stat;
}

auto remove_qualifier(std::string const & stat) -> std::string {
   return remove_tentatively(remove_pending(stat));
}


static constexpr char const * EWG_ACTIVE {"ewg-active.html" };
static constexpr char const * EWG_CLOSED {"ewg-closed.html" };
static constexpr char const * EWG_DEFECTS{"ewg-complete.html"};

// functions to relate the status of an issue to its relevant published list document
auto filename_for_status(std::string stat) -> std::string {
   // Tentative issues are always active
   if(0 == stat.find("Tentatively")) {
      return EWG_ACTIVE;
   }

   stat = remove_qualifier(stat);
   return (stat == "TC1")           ? EWG_DEFECTS
        : (stat == "CD1")           ? EWG_DEFECTS
        : (stat == "C++11")         ? EWG_DEFECTS
        : (stat == "C++14")         ? EWG_DEFECTS
        : (stat == "WP")            ? EWG_DEFECTS
        : (stat == "Resolved")      ? EWG_DEFECTS
        : (stat == "DR")            ? EWG_DEFECTS
        : (stat == "TRDec")         ? EWG_DEFECTS
        : (stat == "Dup")           ? EWG_CLOSED
        : (stat == "NAD")           ? EWG_CLOSED
        : (stat == "NAD Future")    ? EWG_CLOSED
        : (stat == "NAD Editorial") ? EWG_CLOSED
        : (stat == "NAD Concepts")  ? EWG_CLOSED
        : (stat == "Voting")        ? EWG_ACTIVE
        : (stat == "Immediate")     ? EWG_ACTIVE
        : (stat == "Ready")         ? EWG_ACTIVE
        : (stat == "Review")        ? EWG_ACTIVE
        : (stat == "New")           ? EWG_ACTIVE
        : (stat == "Open")          ? EWG_ACTIVE
        : (stat == "Deferred")      ? EWG_ACTIVE
        : throw std::runtime_error("unknown status " + stat);
}

auto is_active(std::string const & stat) -> bool {
   return filename_for_status(stat) == EWG_ACTIVE;
}

auto is_active_not_ready(std::string const & stat) -> bool {
   return is_active(stat)  and  stat != "Ready";
}

auto is_defect(std::string const & stat) -> bool {
   return filename_for_status(stat) == EWG_DEFECTS;
}

auto is_closed(std::string const & stat) -> bool {
   return filename_for_status(stat) == EWG_CLOSED;
}

auto is_tentative(std::string const & stat) -> bool {
   // a more efficient implementation will use some variation of strcmp
   return 0 == stat.find("Tentatively");
}

auto is_not_resolved(std::string const & stat) -> bool {
   for( auto s : {"Deferred", "New", "Open", "Review"}) { if(s == stat) return true; }
   return false;
}

auto is_votable(std::string stat) -> bool {
   stat = remove_tentatively(stat);
   for( auto s : {"Immediate", "Voting"}) { if(s == stat) return true; }
   return false;
}

auto is_ready(std::string stat) -> bool {
   return "Ready" == remove_tentatively(stat);
}

struct section_num {
   std::string       prefix;
   std::vector<int>  num;
};


auto operator >> (std::istream& is, section_num& sn) -> std::istream & {
   sn.prefix.clear();
   sn.num.clear();
   ws(is);
   if (is.peek() == 'T') {
      is.get();
      if (is.peek() == 'R') {
         std::string w;
         is >> w;
         if (w == "R1") {
            sn.prefix = "TR1";
         }
         else if (w == "RDecimal") {
            sn.prefix = "TRDecimal";
         }
         else {
            throw std::runtime_error{"section_num format error"};
         }
         ws(is);
      }
      else {
         sn.num.push_back(100 + 'T' - 'A');
         if (is.peek() != '.') {
            return is;
         }
         is.get();
      }
   }

   while (true) {
      if (std::isdigit(is.peek())) {
         int n;
         is >> n;
         sn.num.push_back(n);
      }
      else {
         char c;
         is >> c;
         sn.num.push_back(100 + c - 'A');
      }
      if (is.peek() != '.') {
         break;
      }
      char dot;
      is >> dot;
   }
   return is;
}

auto operator << (std::ostream& os, section_num const & sn) -> std::ostream & {
   if (!sn.prefix.empty()) { os << sn.prefix << " "; }

   bool use_period{false};
   for (auto sub : sn.num ) {
      if(use_period++) {
         os << '.';
      }

      if (sub >= 100) {
         os << char{sub - 100 + 'A'};
      }
      else {
         os << sub;
      }
   }
   return os;
}

auto operator<(section_num const & x, section_num const & y) noexcept -> bool {
   return (x.prefix < y.prefix) ?  true
        : (y.prefix < x.prefix) ? false
        : x.num < y.num;
}

auto operator==(section_num const & x, section_num const & y) noexcept -> bool {
   return (x.prefix != y.prefix)
        ? false
        : x.num == y.num;
}

auto operator!=(section_num const & x, section_num const & y) noexcept -> bool {
   return !(x == y);
}

typedef std::string section_tag;


struct issue {
   int num;
   std::string                stat;
   std::string                title;
   std::vector<section_tag>   tags;
   std::string                submitter;
   greg::date                 date;
   greg::date                 mod_date;
//   std::vector<std::string>   duplicates;
   std::set<std::string>      duplicates;
   std::string                text;
   bool                       has_resolution;

   // struggling with implicitly declared move operations in GCC 4.6 development compiler
   issue() = default;
   issue(issue const &) = default;
   auto operator=(issue const &) -> issue & = default;

   // Uncomment these lines to test if GCC bug has been fixed (not yet confirmed if issue is compiler or lib)
   // Last vefified: Bug still present in 2010-11-13 build
   //issue(issue &&) = default;
   //auto operator=(issue &&) -> issue & = default;
};


typedef std::map<section_tag, section_num> SectionMap;
SectionMap section_db;


auto remove_square_brackets(section_tag const & tag) -> section_tag {
   assert(tag.size() > 2);
   assert(tag.front() == '[');
   assert(tag.back() == ']');
   return tag.substr(1, tag.size()-2);
}


struct sort_by_section {
   auto operator()(issue const & x, issue const & y) const -> bool {
      assert(!x.tags.empty());
      assert(!y.tags.empty());
      return section_db[x.tags.front()] < section_db[y.tags.front()];
   }
};


struct sort_by_num {
    bool operator()(issue const & x, issue const & y) const noexcept   {  return x.num < y.num;   }
    bool operator()(issue const & x, int y)           const noexcept   {  return x.num < y;       }
    bool operator()(int x,           issue const & y) const noexcept   {  return x     < y.num;   }
};


struct sort_by_status {
   auto operator()(const issue& x, const issue& y) const noexcept -> bool {
      static constexpr
      auto get_priority = []( std::string const & stat ) -> unsigned {
         static char const * const status_priority[] {
            "Voting",
            "Tentatively Voting",
            "Immediate",
            "Ready",
            "Tentatively Ready",
            "Tentatively NAD Editorial",
            "Tentatively NAD Future",
            "Tentatively NAD",
            "Review",
            "New",
            "Open",
            "Deferred",
            "Tentatively Resolved",
            "Pending DR",
            "Pending WP",
            "Pending Resolved",
            "Pending NAD Future",
            "Pending NAD Editorial",
            "Pending NAD",
            "NAD Future",
            "DR",
            "WP",
            "CD1",
            "C++11",
            "C++14",
            "TC1",
            "Resolved",
            "TRDec",
            "NAD Editorial",
            "NAD",
            "Dup",
            "NAD Concepts"
         };
         // Don't know why gcc 4.6 rejects this - cannot deduce iterators for 'find_if' algorithm
         //static auto first = std::begin(status_priority);
         //static auto last  = std::end(status_priority);
         //return std::find_if( first, last), [&](char const * str){ return str == stat; } ) - first;

         // Yet gcc 4.6 does work with this that should have identical semantics, other than an lvalue/rvalue switch
         static auto const first = std::begin(status_priority);
         static auto const last  = std::end(status_priority);
         auto const i = std::find_if( first, last, [&](char const * str){ return str == stat; } );
         if(last == i) {
            std::cout << "Unknown status: " << stat << std::endl;
         }
         return i - first;
      };

      return get_priority(x.stat) < get_priority(y.stat);
   }
};


struct sort_by_first_tag {
   bool operator()(issue const & x, issue const & y) const noexcept {
      assert(!x.tags.empty());
      assert(!y.tags.empty());
      return x.tags.front() < y.tags.front();
   }
};


struct equal_issue_num {
    bool operator()(issue const & x, int y) const noexcept {return x.num == y;}
    bool operator()(int x, issue const & y) const noexcept {return x == y.num;}
};


#ifdef DEBUG_SUPPORT
void display(const std::vector<issue>& issues) {
    for (auto const & i : issues) { std::cout << i; }
}
#endif


auto read_section_db(std::string const & path) -> SectionMap {
   auto filename = path + "section.data";
   std::ifstream infile{filename.c_str()};
   if (!infile.is_open()) {
      throw std::runtime_error{"Can't open section.data\n"};
   }
   std::cout << "Reading section-tag index from: " << filename << std::endl;

   SectionMap section_db;
   while (infile) {
      ws(infile);
      std::string line;
      getline(infile, line);
      if (!line.empty()) {
         assert(line.back() == ']');
         auto p = line.rfind('[');
         assert(p != std::string::npos);
         section_tag tag = line.substr(p);
         assert(tag.size() > 2);
         assert(tag[0] == '[');
         assert(tag[tag.size()-1] == ']');
         line.erase(p-1);

         section_num num;
         if (tag.find("[trdec.") != std::string::npos) {
            num.prefix = "TRDecimal";
            line.erase(0, 10);
         }
         else if (tag.find("[tr.") != std::string::npos) {
            num.prefix = "TR1";
            line.erase(0, 4);
         }

         std::istringstream temp(line);
         if (!std::isdigit(line[0])) {
            char c;
            temp >> c;
            num.num.push_back(100 + c - 'A');
            temp >> c;
         }

         while (temp) {
            int n;
            temp >> n;
            if (!temp.fail()) {
               num.num.push_back(n);
               char dot;
               temp >> dot;
            }
         }

         section_db[tag] = num;
      }
   }
   return section_db;
}


// Is this simply a debugging aid?
//void check_against_index(SectionMap const & section_db) {
//   for (auto const & elem : section_db ) {
//      std::string temp = elem.first;
//      temp.erase(temp.end()-1);
//      temp.erase(temp.begin());
//      std::cout << temp << ' ' << elem.second << '\n';
//   }
//}


auto make_ref_string(issue const & iss) -> std::string {
   auto temp = itos(iss.num);

   std::string result{"<a href=\""};
   result += filename_for_status(iss.stat);
   result += '#';
   result += temp;
   result += "\">";
   result += temp;
   result += "</a>";
   return result;
}

//void synchronize_dupicates(std::vector<issue> & issues) {
//   for( auto & iss : issues ) {
//      if(iss.duplicates.empty()) {
//         continue;
//      }
//
//      // This self-reference looks worth preserving, as can be used to substitute in revision history etc.
//      std::string self_ref = make_ref_string(iss);
//
//      for( auto const & dup : iss.duplicates ) {
//         n->duplicates.insert(self_ref); // This is only reason vector is not const
//         is.duplicates.insert(make_ref_string(*n));
//         r.clear();
//      }
//   }
//}

void replace_all_irefs(std::vector<issue> const & issues, std::string & s) {
   // Replace all tagged "issues references" in string 's' with an HTML anchor-link to the live issue
   // in its appropriate issue list, as determined by the issue's status.
   // Format of an issue reference: <iref ref="ISS"/>
   // Format of anchor: <a href="ewg-INDEX.html#ISS">ISS</a>

   for (auto i = s.find("<iref ref=\""); i != std::string::npos; i = s.find("<iref ref=\"") ) {
      auto j = s.find('>', i);
      if (j == std::string::npos) {
         throw std::runtime_error{"missing '>' after iref"};
      }

      auto k = s.find('\"', i+5);
      if (k >= j) {
         throw std::runtime_error{"missing '\"' in iref"};
      }
      auto l = s.find('\"', k+1);
      if (l >= j) {
         throw std::runtime_error{"missing '\"' in iref"};
      }

      ++k;

      std::istringstream temp{s.substr(k, l-k)};
      int num;
      temp >> num;
      if (temp.fail()) {
         throw std::runtime_error{"bad number in iref"};
      }

      auto n = std::lower_bound(issues.begin(), issues.end(), num, sort_by_num{});
      if (n->num != num) {
         throw std::runtime_error{"couldn't find number in iref"};
      }

      std::string r{make_ref_string(*n)};
      j -= i - 1;
      s.replace(i, j, r);
      i += r.size() - 1;
   }
}


void format(std::vector<issue> & issues, issue & is) {
   std::string & s = is.text;
   int issue_num = is.num;
   std::vector<std::string> tag_stack;
   std::ostringstream er;
   // cannot rewrite as range-based for-loop as the string 's' is modified within the loop
   for (unsigned i = 0; i < s.size(); ++i) {
      if (s[i] == '<') {
         auto j = s.find('>', i);
         if (j == std::string::npos) {
            er.clear();
            er.str("");
            er << "missing '>' in issue " << issue_num;
            throw std::runtime_error{er.str()};
         }

         std::string tag;
         {
            std::istringstream iword{s.substr(i+1, j-i-1)};
            iword >> tag;
         }

         if (tag.empty()) {
             er.clear();
             er.str("");
             er << "unexpected <> in issue " << issue_num;
             throw std::runtime_error{er.str()};
         }

         if (tag[0] == '/') { // closing tag
             tag.erase(tag.begin());
             if (tag == "issue" || tag == "revision") {
                s.erase(i, j-i + 1);
                --i;
                return;
             }

             if (tag_stack.empty()  or  tag != tag_stack.back()) {
                er.clear();
                er.str("");
                er << "mismatched tags in issue " << issue_num;
                if (tag_stack.empty()) {
                   er << ".  Had no open tag.";
                }
                else {
                   er << ".  Open tag was " << tag_stack.back() << ".";
                }
                er << "  Closing tag was " << tag;
                throw std::runtime_error{er.str()};
             }

             tag_stack.pop_back();
             if (tag == "discussion") {
                 s.erase(i, j-i + 1);
                 --i;
             }
             else if (tag == "resolution") {
                 s.erase(i, j-i + 1);
                 --i;
             }
             else if (tag == "rationale") {
                 s.erase(i, j-i + 1);
                 --i;
             }
             else if (tag == "duplicate") {
                 s.erase(i, j-i + 1);
                 --i;
             }
             else if (tag == "note") {
                 s.replace(i, j-i + 1, "]</i></p>\n");
                 i += 9;
             }
             else {
                 i = j;
             }

             continue;
         }

         if (s[j-1] == '/') { // self-contained tag: sref, iref
            if (tag == "sref") {
               static const
               auto report_missing_quote = [](std::ostringstream & er, unsigned num) {
                  er.clear();
                  er.str("");
                  er << "missing '\"' in sref in issue " << num;
                  throw std::runtime_error{er.str()};
               };

               std::string r;
               auto k = s.find('\"', i+5);
               if (k >= j) {
                  report_missing_quote(er, issue_num);
               }

               auto l = s.find('\"', k+1);
               if (l >= j) {
                  report_missing_quote(er, issue_num);
               }

               ++k;
               r = s.substr(k, l-k);
               {
                  std::ostringstream t;
                  t << section_db[r] << ' ';
                  r.insert(0, t.str());
               }

               j -= i - 1;
               s.replace(i, j, r);
               i += r.size() - 1;
               continue;
            }
            else if (tag == "iref") {
               static const
               auto report_missing_quote = [](std::ostringstream & er, unsigned num) {
                  er.clear();
                  er.str("");
                  er << "missing '\"' in iref in issue " << num;
                  throw std::runtime_error{er.str()};
               };

               auto k = s.find('\"', i+5);
               if (k >= j) {
                  report_missing_quote(er, issue_num);
               }
               auto l = s.find('\"', k+1);
               if (l >= j) {
                  report_missing_quote(er, issue_num);
               }

               ++k;
               std::string r{s.substr(k, l-k)};
               std::istringstream temp{r};
               int num;
               temp >> num;
               if (temp.fail()) {
                  er.clear();
                  er.str("");
                  er << "bad number in iref in issue " << issue_num;
                  throw std::runtime_error{er.str()};
               }

               auto n = std::lower_bound(issues.begin(), issues.end(), num, sort_by_num{});
               if (n->num != num) {
                  er.clear();
                  er.str("");
                  er << "couldn't find number in iref in issue " << issue_num;
                  throw std::runtime_error{er.str()};
               }

               if (!tag_stack.empty()  and  tag_stack.back() == "duplicate") {
                  n->duplicates.insert(make_ref_string(is)); // This is only reason vector is not const
                  is.duplicates.insert(make_ref_string(*n));
                  r.clear();
               }
               else {
                  r = make_ref_string(*n);
               }

               j -= i - 1;
               s.replace(i, j, r);
               i += r.size() - 1;
               continue;
            }
            i = j;
            continue;  // don't worry about this <tag/>
         }

         tag_stack.push_back(tag);
         if (tag == "discussion") {
             s.replace(i, j-i + 1, "<p><b>Discussion:</b></p>");
             i += 24;
         }
         else if (tag == "resolution") {
             s.replace(i, j-i + 1, "<p><b>Wording available:</b></p>");
             i += 30;
         }
         else if (tag == "rationale") {
             s.replace(i, j-i + 1, "<p><b>Rationale:</b></p>");
             i += 23;
         }
         else if (tag == "duplicate") {
             s.erase(i, j-i + 1);
             --i;
         }
         else if (tag == "note") {
             s.replace(i, j-i + 1, "<p><i>[");
             i += 6;
         }
         else if (tag == "!--") {
             tag_stack.pop_back();
             j = s.find("-->", i);
             j += 3;
             s.erase(i, j-i);
             --i;
         }
         else {
             i = j;
         }
      }
   }
}


struct EwgIssuesXml {
   explicit EwgIssuesXml(std::string const & path);

   auto get_doc_number(std::string doc) const -> std::string;
   auto get_intro(std::string doc) const -> std::string;
   auto get_maintainer() const -> std::string;
   auto get_revision() const -> std::string;
   auto get_revisions(std::vector<issue> const & issues, std::string const & diff_report) const -> std::string;
   auto get_statuses() const -> std::string;

private:
   std::string m_data;
};

EwgIssuesXml::EwgIssuesXml(std::string const & path)
   : m_data{}
   {
   std::ifstream infile{(path + "ewg-issues.xml").c_str()};
   if (!infile.is_open()) {
      throw std::runtime_error{"Unable to open ewg-issues.xml"};
   }

   std::istreambuf_iterator<char> first{infile}, last{};
   m_data.assign(first, last);
}


auto EwgIssuesXml::get_doc_number(std::string doc) const -> std::string {
    if (doc == "active") {
        doc = "active_docno=\"";
    }
    else if (doc == "complete") {
        doc = "defect_docno=\"";
    }
    else if (doc == "closed") {
        doc = "closed_docno=\"";
    }
    else {
        throw std::runtime_error{"unknown argument to get_doc_number: " + doc};
    }

    auto i = m_data.find(doc);
    if (i == std::string::npos) {
        throw std::runtime_error{"Unable to find docno in ewg-issues.xml"};
    }
    i += doc.size();
    auto j = m_data.find('\"', i+1);
    if (j == std::string::npos) {
        throw std::runtime_error{"Unable to parse docno in ewg-issues.xml"};
    }
    return m_data.substr(i, j-i);
}

auto EwgIssuesXml::get_intro(std::string doc) const -> std::string {
    if (doc == "active") {
        doc = "<intro list=\"Active\">";
    }
    else if (doc == "complete") {
        doc = "<intro list=\"Complete\">";
    }
    else if (doc == "closed") {
        doc = "<intro list=\"Closed\">";
    }
    else {
        throw std::runtime_error{"unknown argument to intro: " + doc};
    }

    auto i = m_data.find(doc);
    if (i == std::string::npos) {
        throw std::runtime_error{"Unable to find intro in ewg-issues.xml"};
    }
    i += doc.size();
    auto j = m_data.find("</intro>", i);
    if (j == std::string::npos) {
        throw std::runtime_error{"Unable to parse intro in ewg-issues.xml"};
    }
    return m_data.substr(i, j-i);
}


auto EwgIssuesXml::get_maintainer() const -> std::string {
   auto i = m_data.find("maintainer=\"");
   if (i == std::string::npos) {
      throw std::runtime_error{"Unable to find maintainer in ewg-issues.xml"};
   }
   i += sizeof("maintainer=\"") - 1;
   auto j = m_data.find('\"', i);
   if (j == std::string::npos) {
      throw std::runtime_error{"Unable to parse maintainer in ewg-issues.xml"};
   }
   std::string r = m_data.substr(i, j-i);
   auto m = r.find("&lt;");
   if (m == std::string::npos) {
      throw std::runtime_error{"Unable to parse maintainer email address in ewg-issues.xml"};
   }
   m += sizeof("&lt;") - 1;
   auto me = r.find("&gt;", m);
   if (me == std::string::npos) {
      throw std::runtime_error{"Unable to parse maintainer email address in ewg-issues.xml"};
   }
   std::string email = r.substr(m, me-m);
   // &lt;                                    ewgchair@gmail.com    &gt;
   // &lt;<a href="mailto:ewgchair@gmail.com">ewgchair@gmail.com</a>&gt;
   r.replace(m, me-m, "<a href=\"mailto:" + email + "\">" + email + "</a>");
   return r;
}


auto EwgIssuesXml::get_revision() const -> std::string {
    auto i = m_data.find("revision=\"");
    if (i == std::string::npos) {
        throw std::runtime_error{"Unable to find revision in ewg-issues.xml"};
    }
    i += sizeof("revision=\"") - 1;
    auto j = m_data.find('\"', i);
    if (j == std::string::npos) {
        throw std::runtime_error{"Unable to parse revision in ewg-issues.xml"};
    }
    return m_data.substr(i, j-i);
}


auto EwgIssuesXml::get_revisions(std::vector<issue> const & issues, std::string const & diff_report) const -> std::string {
   auto i = m_data.find("<revision_history>");
   if (i == std::string::npos) {
      throw std::runtime_error{"Unable to find <revision_history> in ewg-issues.xml"};
   }
   i += sizeof("<revision_history>") - 1;

   auto j = m_data.find("</revision_history>", i);
   if (j == std::string::npos) {
      throw std::runtime_error{"Unable to find </revision_history> in ewg-issues.xml"};
   }
   auto s = m_data.substr(i, j-i);
   j = 0;

   // bulding a potentially large string, would a stringstream be a better solution?
   // Probably not - string will not be *that* big, and stringstream pays the cost of locales
   std::string r = "<ul>\n";

   r += "<li>";
//   r += "R74: 2011-02-28 pre-Madrid mailing";   // This is the form we are copying
//   r += "R01: 2013-03-18 pre-Bristol mailing";   // We should date and *timestamp* this reference, as we expect to generate several documents per day
//   r += "D02: 2013-04-28 updated tiny issue statuses";   // We should date and *timestamp* this reference, as we expect to generate several documents per day
//   r += "R02: 2013-05-06 post-Bristol mailing";   // We should date and *timestamp* this reference, as we expect to generate several documents per day
//     r += "R03: 2013-08-27 pre-Chicago mailing";   // We should date and *timestamp* this reference, as we expect to generate several documents per day
//     r += "R04: 2013-10-11 post-Chicago mailing";   // We should date and *timestamp* this reference, as we expect to generate several documents per day
//     r += "R05: 2014-01-17 pre-Issaquah mailing";   // We should date and *timestamp* this reference, as we expect to generate several documents per day
//     r += "R06: 2014-02-21 post-Issaquah mailing";   // We should date and *timestamp* this reference, as we expect to generate several documents per day
//     r += "R07: 2014-05-20 pre-Rapperswil mailing";   // We should date and *timestamp* this reference, as we expect to generate several documents per day
//     r += "D08: 2014-06-01 working draft";   // We should date and *timestamp* this reference, as we expect to generate several documents per day
//       r += "R08: 2014-07-02 post-Rapperswil mailing";
//   r += "D09: 2014-07-02 working version";
//   r += "R09: 2014-10-09 pre-Urbana mailing";
//   r += "R10: 2014-10-29 post-Urbana mailing";
   r += "R11: 2015-04-10 pre-Lenexa mailing";
//   r += "Madrid meeting resolutions";   // We should date and *timestamp* this reference, as we expect to generate several documents per day
   r += diff_report;
   r += "</li>\n";

   while (true) {
      i = s.find("<revision tag=\"", j);
      if (i == std::string::npos) {
         break;
      }
      i += sizeof("<revision tag=\"") - 1;
      j = s.find('\"', i);
      std::string const rv = s.substr(i, j-i);
      i = j+2;
      j = s.find("</revision>", i);

      r += "<li>";
      r += rv + ": ";
      r += s.substr(i, j-i);
      r += "</li>\n";
   }
   r += "</ul>\n";

   replace_all_irefs(issues, r);

   return r;
}


auto EwgIssuesXml::get_statuses() const -> std::string {
   auto i = m_data.find("<statuses>");
   if (i == std::string::npos) {
      throw std::runtime_error{"Unable to find statuses in ewg-issues.xml"};
   }
   i += sizeof("<statuses>") - 1;

   auto j = m_data.find("</statuses>", i);
   if (j == std::string::npos) {
      throw std::runtime_error{"Unable to parse statuses in ewg-issues.xml"};
   }
   return m_data.substr(i, j-i);
}


void print_file_header(std::ostream& out, std::string const & title) {
   out <<
R"(<!DOCTYPE HTML PUBLIC "-//W3C//DTD HTML 4.01//EN"
    "http://www.w3.org/TR/html4/strict.dtd">
<html>
<head>
<title>)" << title << R"(</title>
<style type="text/css">
  p {text-align:justify}
  li {text-align:justify}
  blockquote.note
  {
    background-color:#E0E0E0;
    padding-left: 15px;
    padding-right: 15px;
    padding-top: 1px;
    padding-bottom: 1px;
  }
  ins {background-color:#A0FFA0}
  del {background-color:#FFA0A0}
</style>
</head>
<body>
)";

}


void print_file_trailer(std::ostream& out) {
    out << "</body>\n";
    out << "</html>\n";
}


void print_table(std::ostream& out, std::vector<issue>::const_iterator i, std::vector<issue>::const_iterator e) {
#if defined (DEBUG_LOGGING)
   std::cout << "\t" << std::distance(i,e) << " items to add to table" << std::endl;
#endif

   out <<
R"(<table border="1" cellpadding="4">
<tr>
  <td align="center"><a href="ewg-toc.html"><b>Issue</b></a></td>
  <td align="center"><a href="ewg-status.html"><b>Status</b></a></td>
  <td align="center"><a href="ewg-index.html"><b>Section</b></a></td>
  <td align="center"><b>Title</b></td>
  <td align="center"><b>Wording available</b></td>
  <td align="center"><b>Duplicates</b></td>
  <td align="center"><a href="ewg-status-date.html"><b>Last modified</b></a></td>
</tr>
)";

   std::string prev_tag;
   for (; i != e; ++i) {
      out << "<tr>\n";

      // Number
      out << "<td align=\"right\">" << make_ref_string(*i) << "</td>\n";

      // Status
      out << "<td align=\"left\"><a href=\"ewg-active.html#" << remove_qualifier(i->stat) << "\">" << i->stat << "</a><a name=\"" << i->num << "\"></a></td>\n";

      // Section
      out << "<td align=\"left\">";
assert(!i->tags.empty());
      out << section_db[i->tags[0]] << " " << i->tags[0];
      if (i->tags[0] != prev_tag) {
         prev_tag = i->tags[0];
         out << "<a name=\"" << remove_square_brackets(prev_tag) << "\"</a>";
      }
      out << "</td>\n";

      // Title
      out << "<td align=\"left\">" << i->title << "</td>\n";

      // Has Proposed Resolution
      out << "<td align=\"center\">";
      if (i->has_resolution) {
         out << "Yes";
      }
      else {
         out << "<font color=\"red\">No</font>";
      }
      out << "</td>\n";

      // Duplicates
      out << "<td align=\"left\">";
      print_list(out, i->duplicates, ", ");
      out << "</td>\n";

      // Modification date
      out << "<td align=\"center\">";
      print_date(out, i->mod_date);
      out << "</td>\n"
          << "</tr>\n";
   }
   out << "</table>\n";
}


void make_sort_by_num(std::vector<issue>& issues, std::string const & filename, EwgIssuesXml const & ewg_issues_xml) {
   sort(issues.begin(), issues.end(), sort_by_num{});

   std::ofstream out{filename.c_str()};
   print_file_header(out, "EWG Table of Contents");

   out <<
R"(<h1>C++ Standard Evolution Issues List (Revision )" << ewg_issues_xml.get_revision() << R"()</h1>
<h1>Table of Contents</h1>
<p>Reference ISO/IEC IS 14882:2003(E)</p>
<p>This document is the Table of Contents for the <a href="ewg-active.html">Evolution Active Issues List</a>,
<a href="ewg-complete.html">Evolution Completed Issues List</a>, and <a href="ewg-closed.html">Evolution Closed Issues List</a>.</p>
)";
   out << build_timestamp;

   print_table(out, issues.begin(), issues.end());
   print_file_trailer(out);
}


void make_sort_by_status(std::vector<issue>& issues, std::string const & filename, EwgIssuesXml const & ewg_issues_xml) {
   sort(issues.begin(), issues.end(), sort_by_num{});
   stable_sort(issues.begin(), issues.end(), [](issue const & x, issue const & y) { return x.mod_date > y.mod_date; } );
   stable_sort(issues.begin(), issues.end(), sort_by_section{});
   stable_sort(issues.begin(), issues.end(), sort_by_status{});

   std::ofstream out{filename.c_str()};
   print_file_header(out, "EWG Index by Status and Section");

   out <<
R"(<h1>C++ Standard Evolution Issues List (Revision )" << ewg_issues_xml.get_revision() << R"()</h1>
<h1>Index by Status and Section</h1>
<p>Reference ISO/IEC IS 14882:2003(E)</p>
<p>
This document is the Index by Status and Section for the <a href="ewg-active.html">Evolution Active Issues List</a>,
<a href="ewg-complete.html">Evolution Completed Issues List</a>, and <a href="ewg-closed.html">Evolution Closed Issues List</a>.
</p>

)";
   out << build_timestamp;

   for (auto i = issues.cbegin(), e = issues.cend(); i != e;) {
      auto const & current_status = i->stat;
      auto j = std::find_if(i, e, [&](issue const & iss){ return iss.stat != current_status; } );
      out << "<h2><a name=\"" << current_status << "\"</a>" << current_status << " (" << (j-i) << " issues)</h2>\n";
      print_table(out, i, j);
      i = j;
   }

   print_file_trailer(out);
}


void make_sort_by_status_mod_date(std::vector<issue> & issues, std::string const & filename, EwgIssuesXml const & ewg_issues_xml) {
   sort(issues.begin(), issues.end(), sort_by_num{});
   stable_sort(issues.begin(), issues.end(), sort_by_section{});
   stable_sort(issues.begin(), issues.end(), [](issue const & x, issue const & y) { return x.mod_date > y.mod_date; } );
   stable_sort(issues.begin(), issues.end(), sort_by_status{});

   std::ofstream out{filename.c_str()};
   print_file_header(out, "EWG Index by Status and Date");

   out <<
R"(<h1>C++ Standard Evolution Issues List (Revision )" << ewg_issues_xml.get_revision() << R"()</h1>
<h1>Index by Status and Date</h1>
<p>Reference ISO/IEC IS 14882:2003(E)</p>
<p>
This document is the Index by Status and Date for the <a href="ewg-active.html">Evolution Active Issues List</a>,
<a href="ewg-complete.html">Evolution Completed Issues List</a>, and <a href="ewg-closed.html">Evolution Closed Issues List</a>.
</p>
)";
   out << build_timestamp;

   for (auto i = issues.cbegin(), e = issues.cend(); i != e;) {
      std::string const & current_status = i->stat;
      auto j = find_if(i, e, [&](issue const & iss){ return iss.stat != current_status; } );
      out << "<h2><a name=\"" << current_status << "\"</a>" << current_status << " (" << (j-i) << " issues)</h2>\n";
      print_table(out, i, j);
      i = j;
   }

   print_file_trailer(out);
}


auto major_section(section_num const & sn) -> std::string {
   std::string const prefix{sn.prefix};
   std::ostringstream out;
   if (!prefix.empty()) {
      out << prefix << " ";
   }
   if (sn.num[0] < 100) {
      out << sn.num[0];
   }
   else {
      out << char{sn.num[0] - 100 + 'A'};
   }
   return out.str();
}

struct sort_by_mjr_section {
    auto operator()(const issue& x, const issue& y) const -> bool {
assert(!x.tags.empty());
assert(!y.tags.empty());
        section_num const & xn = section_db[x.tags[0]];
        section_num const & yn = section_db[y.tags[0]];
        return  xn.prefix < yn.prefix
            or (xn.prefix > yn.prefix  and  xn.num[0] < yn.num[0]);
    }
};

void make_sort_by_section(std::vector<issue>& issues, std::string const & filename, EwgIssuesXml const & ewg_issues_xml, bool active_only = false) {
   sort(issues.begin(), issues.end(), sort_by_num{});
   stable_sort(issues.begin(), issues.end(), [](issue const & x, issue const & y) { return x.mod_date > y.mod_date; } );
   stable_sort(issues.begin(), issues.end(), sort_by_status{});
   auto b = issues.begin();
   auto e = issues.end();
   if(active_only) {
      auto bReady = find_if(b, e, [](issue const & iss){ return "Ready" == iss.stat; });
      if(bReady != e) {
         b = bReady;
      }
      b = find_if(b, e, [](issue const & iss){ return "Ready" != iss.stat; });
      e = find_if(b, e, [](issue const & iss){ return !is_active(iss.stat); });
   }
   stable_sort(b, e, sort_by_section{});
   std::set<issue, sort_by_mjr_section> mjr_section_open;
   for (auto const & elem : issues ) {
      if (is_active_not_ready(elem.stat)) {
         mjr_section_open.insert(elem);
      }
   }

   std::ofstream out(filename.c_str());
   print_file_header(out, "EWG Index by Section");

   out << "<h1>C++ Standard Evolution Issues List (Revision " << ewg_issues_xml.get_revision() << ")</h1>\n";
   out << "<h1>Index by Section</h1>\n";
   out << "<p>Reference ISO/IEC IS 14882:2003(E)</p>\n";
   out << "<p>This document is the Index by Section for the <a href=\"ewg-active.html\">Evolution Active Issues List</a>";
   if(!active_only) {
      out << ", <a href=\"ewg-complete.html\">Evolution Completed Issues List</a>, and <a href=\"ewg-closed.html\">Evolution Closed Issues List</a>";
   }
   out << ".</p>\n";
   out << "<h2>Index by Section";
   if (active_only) {
      out << " (non-Ready active issues only)";
   }
   out << "</h2>\n";
   if (active_only) {
      out << "<p><a href=\"ewg-index.html\">(view all issues)</a></p>\n";
   }
   else {
      out << "<p><a href=\"ewg-index-open.html\">(view only non-Ready open issues)</a></p>\n";
   }
   out << build_timestamp;

   // Would prefer to use const_iterators from here, but oh well....
   for (auto i = b; i != e;) {
assert(!i->tags.empty());
      int current_num = section_db[i->tags[0]].num[0];
      auto j = i;
      for (; j != e; ++j) {
         if (section_db[j->tags[0]].num[0] != current_num) {
             break;
         }
      }
      std::string const msn{major_section(section_db[i->tags[0]])};
      out << "<h2><a name=\"Section " << msn << "\"></a>" << "Section " << msn << " (" << (j-i) << " issues)</h2>\n";
      if (active_only) {
         out << "<p><a href=\"ewg-index.html#Section " << msn << "\">(view all issues)</a></p>\n";
      }
      else if (mjr_section_open.count(*i) > 0) {
         out << "<p><a href=\"ewg-index-open.html#Section " << msn << "\">(view only non-Ready open issues)</a></p>\n";
      }

      print_table(out, i, j);
      i = j;
   }

   print_file_trailer(out);
}


template <class Pred>
void print_issues(std::ostream & out, std::vector<issue> const & issues, Pred pred) {
   std::multiset<issue, sort_by_first_tag> const  all_issues{ issues.begin(), issues.end()} ;
   std::multiset<issue, sort_by_status>    const  issues_by_status{ issues.begin(), issues.end() };

   std::multiset<issue, sort_by_first_tag> active_issues;
   for (auto const & elem : issues ) {
      if (is_active(elem.stat)) {
         active_issues.insert(elem);
      }
   }

   for (auto const & iss : issues) {
      if (pred(iss)) {
         out << "<hr>\n";

         // Number and title
         out << "<h3><a name=\"" << iss.num << "\"></a>" << iss.num << ". " << iss.title << "</h3>\n";

         // Section, Status, Submitter, Date
         out << "<p><b>Section:</b> ";
         out << section_db[iss.tags[0]] << " " << iss.tags[0];
         for (unsigned k = 1; k < iss.tags.size(); ++k) {
            out << ", " << section_db[iss.tags[k]] << " " << iss.tags[k];
         }

         out << " <b>Status:</b> <a href=\"ewg-active.html#" << remove_qualifier(iss.stat) << "\">" << iss.stat << "</a>\n";
         out << " <b>Submitter:</b> " << iss.submitter
             << " <b>Opened:</b> ";
         print_date(out, iss.date);
         out << " <b>Last modified:</b> ";
         print_date(out, iss.mod_date);
         out << "</p>\n";

         // view active issues in []
         if (active_issues.count(iss) > 1) {
            out << "<p><b>View other</b> <a href=\"ewg-index-open.html#" << remove_square_brackets(iss.tags[0]) << "\">active issues</a> in " << iss.tags[0] << ".</p>\n";
         }

         // view all issues in []
         if (all_issues.count(iss) > 1) {
            out << "<p><b>View all other</b> <a href=\"ewg-index.html#" << remove_square_brackets(iss.tags[0]) << "\">issues</a> in " << iss.tags[0] << ".</p>\n";
         }
         // view all issues with same status
         if (issues_by_status.count(iss) > 1) {
            out << "<p><b>View all issues with</b> <a href=\"ewg-status.html#" << iss.stat << "\">" << iss.stat << "</a> status.</p>\n";
         }

         // duplicates
         if (!iss.duplicates.empty()) {
            out << "<p><b>Duplicate of:</b> ";
            print_list(out, iss.duplicates, ", ");
            out << "</p>\n";
         }

         // text
         out << iss.text << "\n\n";
      }
   }
}

void print_paper_heading(std::ostream& out, std::string const & paper, EwgIssuesXml const & ewg_issues_xml) {
   out <<
R"(<table>
<tr>
  <td align="left">Doc. no.</td>
  <td align="left">)" << ewg_issues_xml.get_doc_number(paper) << R"(</td>
</tr>
<tr>
  <td align="left">Date:</td>
  <td align="left">)" << format_time("%Y-%m-%d", utc_timestamp()) << R"(</td>
</tr>
<tr>
  <td align="left">Project:</td>
  <td align="left">Programming Language C++</td>
</tr>
<tr>
  <td align="left">Reply to:</td>
  <td align="left">)" << ewg_issues_xml.get_maintainer() << R"(</td>
</tr>
</table>
)";

   out << "<h1>";
   if (paper == "active") {
      out << "C++ Standard Evolution Active Issues List (Revision ";
   }
   else if (paper == "complete") {
      out << "C++ Standard Evolution Completed Issues List (Revision ";
   }
   else if (paper == "closed") {
      out << "C++ Standard Evolution Closed Issues List (Revision ";
   }
   out << ewg_issues_xml.get_revision() << ")</h1>\n";
   out << build_timestamp;
}


// Functions to make the 3 standard published issues list documents
// A precondition for calling any of these functions is that the list of issues is sorted in numerical order, by issue number.
// While nothing disasterous will happen if this precondition is violated, the published issues list will list items
// in the wrong order.
void make_active(std::vector<issue> const & issues, std::string const & path, EwgIssuesXml const & ewg_issues_xml, std::string const & diff_report) {
   assert(is_sorted(issues.begin(), issues.end(), sort_by_num{}));

   std::ofstream out{(path + "ewg-active.html").c_str()};
   print_file_header(out, "C++ Standard Evolution Active Issues List");
   print_paper_heading(out, "active", ewg_issues_xml);
   out << ewg_issues_xml.get_intro("active") << '\n';
   out << "<h2>Revision History</h2>\n" << ewg_issues_xml.get_revisions(issues, diff_report) << '\n';
   out << "<h2><a name=\"Status\"></a>Issue Status</h2>\n" << ewg_issues_xml.get_statuses() << '\n';
   out << "<h2>Active Issues</h2>\n";
   print_issues(out, issues, [](issue const & i) {return is_active(i.stat);} );
   print_file_trailer(out);
}


void make_defect(std::vector<issue> const & issues, std::string const & path, EwgIssuesXml const & ewg_issues_xml, std::string const & diff_report) {
   assert(is_sorted(issues.begin(), issues.end(), sort_by_num{}));

   std::ofstream out((path + "ewg-complete.html").c_str());
   print_file_header(out, "C++ Standard Evolution Completed Issues List");
   print_paper_heading(out, "complete", ewg_issues_xml);
   out << ewg_issues_xml.get_intro("complete") << '\n';
   out << "<h2>Revision History</h2>\n" << ewg_issues_xml.get_revisions(issues, diff_report) << '\n';
   out << "<h2>Completed Issues</h2>\n";
   print_issues(out, issues, [](issue const & i) {return is_defect(i.stat);} );
   print_file_trailer(out);
}


void make_closed(std::vector<issue> const & issues, std::string const & path, EwgIssuesXml const & ewg_issues_xml, std::string const & diff_report) {
   assert(is_sorted(issues.begin(), issues.end(), sort_by_num{}));

   std::ofstream out{(path + "ewg-closed.html").c_str()};
   print_file_header(out, "C++ Standard Evolution Closed Issues List");
   print_paper_heading(out, "closed", ewg_issues_xml);
   out << ewg_issues_xml.get_intro("closed") << '\n';
   out << "<h2>Revision History</h2>\n" << ewg_issues_xml.get_revisions(issues, diff_report) << '\n';
   out << "<h2>Closed Issues</h2>\n";
   print_issues(out, issues, [](issue const & i) {return is_closed(i.stat);} );
   print_file_trailer(out);
}


// Additional non-standard documents, useful for running EWG meetings
void make_tentative(std::vector<issue> const & issues, std::string const & path, EwgIssuesXml const & ewg_issues_xml) {
   // publish a document listing all tentative issues that may be acted on during a meeting.
   assert(is_sorted(issues.begin(), issues.end(), sort_by_num{}));

   std::ofstream out{(path + "ewg-tentative.html").c_str()};
   print_file_header(out, "C++ Standard Evolution Tentative Issues");
//   print_paper_heading(out, "active", ewg_issues_xml);
//   out << ewg_issues_xml.get_intro("active") << '\n';
//   out << "<h2>Revision History</h2>\n" << ewg_issues_xml.get_revisions(issues) << '\n';
//   out << "<h2><a name=\"Status\"></a>Issue Status</h2>\n" << ewg_issues_xml.get_statuses() << '\n';
   out << build_timestamp;
   out << "<h2>Tentative Issues</h2>\n";
   print_issues(out, issues, [](issue const & i) {return is_tentative(i.stat);} );
   print_file_trailer(out);
}


void make_unresolved(std::vector<issue> const & issues, std::string const & path, EwgIssuesXml const & ewg_issues_xml) {
   // publish a document listing all non-tentative, non-ready issues that must be reviewed during a meeting.
   assert(is_sorted(issues.begin(), issues.end(), sort_by_num{}));

   std::ofstream out{(path + "ewg-unresolved.html").c_str()};
   print_file_header(out, "C++ Standard Evolution Unresolved Issues");
//   print_paper_heading(out, "active", ewg_issues_xml);
//   out << ewg_issues_xml.get_intro("active") << '\n';
//   out << "<h2>Revision History</h2>\n" << ewg_issues_xml.get_revisions(issues) << '\n';
//   out << "<h2><a name=\"Status\"></a>Issue Status</h2>\n" << ewg_issues_xml.get_statuses() << '\n';
   out << build_timestamp;
   out << "<h2>Unresolved Issues</h2>\n";
   print_issues(out, issues, [](issue const & i) {return is_not_resolved(i.stat);} );
   print_file_trailer(out);
}

void make_immediate(std::vector<issue> const & issues, std::string const & path, EwgIssuesXml const & ewg_issues_xml) {
   // publish a document listing all non-tentative, non-ready issues that must be reviewed during a meeting.
   assert(is_sorted(issues.begin(), issues.end(), sort_by_num{}));

   std::ofstream out{(path + "ewg-immediate.html").c_str()};
   print_file_header(out, "C++ Standard Evolution Issues Resolved Directly In Madrid");
//   print_paper_heading(out, "active", ewg_issues_xml);
//   out << ewg_issues_xml.get_intro("active") << '\n';
//   out << "<h2>Revision History</h2>\n" << ewg_issues_xml.get_revisions(issues) << '\n';
//   out << "<h2><a name=\"Status\"></a>Issue Status</h2>\n" << ewg_issues_xml.get_statuses() << '\n';
   out << build_timestamp;
   out << "<h2>Immediate Issues</h2>\n";
   print_issues(out, issues, [](issue const & i) {return "Immediate" == i.stat;} );
   print_file_trailer(out);
}


auto parse_issue_from_file(std::string const & filename) -> issue {
   struct bad_issue_file : std::runtime_error {
      bad_issue_file(std::string const & filename, char const * error_message)
         : runtime_error{"Error parsing issue file " + filename + ": " + error_message}
         {
      }
   };

   issue is;
   is.text = read_file_into_string(filename);
   auto & tx = is.text; // saves a redundant copy at the end of the function, while preserving existing names and logic

   // Get issue number
   auto k = tx.find("<issue num=\"");
   if (k == std::string::npos) {
      throw bad_issue_file{filename, "Unable to find issue number"};
   }
   k += sizeof("<issue num=\"") - 1;
   auto l = tx.find('\"', k);
   std::istringstream temp{tx.substr(k, l-k)};
   temp >> is.num;

   // Get issue status
   k = tx.find("status=\"", l);
   if (k == std::string::npos) {
      throw bad_issue_file{filename, "Unable to find issue status"};
   }
   k += sizeof("status=\"") - 1;
   l = tx.find('\"', k);
   is.stat = tx.substr(k, l-k);

   // Get issue title
   k = tx.find("<title>", l);
   if (k == std::string::npos) {
      throw bad_issue_file{filename, "Unable to find issue title"};
   }
   k +=  sizeof("<title>") - 1;
   l = tx.find("</title>", k);
   is.title = tx.substr(k, l-k);

   // Get issue sections
   k = tx.find("<section>", l);
   if (k == std::string::npos) {
      throw bad_issue_file{filename, "Unable to find issue section"};
   }
   k += sizeof("<section>") - 1;
   l = tx.find("</section>", k);
   while (k < l) {
      k = tx.find('\"', k);
      if (k >= l) {
          break;
      }
      auto k2 = tx.find('\"', k+1);
      if (k2 >= l) {
         throw bad_issue_file{filename, "Unable to find issue section"};
      }
      ++k;
      is.tags.push_back(tx.substr(k, k2-k));
      if (section_db.find(is.tags.back()) == section_db.end()) {
          section_num num{};
          num.num.push_back(100 + 'X' - 'A');
          section_db[is.tags.back()] = num;
      }
      k = k2;
      ++k;
   }

   if (is.tags.empty()) {
      throw bad_issue_file{filename, "Unable to find issue section"};
   }

   // Get submitter
   k = tx.find("<submitter>", l);
   if (k == std::string::npos) {
      throw bad_issue_file{filename, "Unable to find issue submitter"};
   }
   k += sizeof("<submitter>") - 1;
   l = tx.find("</submitter>", k);
   is.submitter = tx.substr(k, l-k);

   // Get date
   k = tx.find("<date>", l);
   if (k == std::string::npos) {
      throw bad_issue_file{filename, "Unable to find issue date"};
   }
   k += sizeof("<date>") - 1;
   l = tx.find("</date>", k);
   temp.clear();
   temp.str(tx.substr(k, l-k));

   try {
      is.date = parse_date(temp);

      // Get modification date
      is.mod_date = report_date_file_last_modified(filename);
   }
   catch(std::exception const & ex) {
      throw bad_issue_file{filename, ex.what()};
   }

   // Trim text to <discussion>
   k = tx.find("<discussion>", l);
   if (k == std::string::npos) {
      throw bad_issue_file{filename, "Unable to find issue discussion"};
   }
   tx.erase(0, k);

   // Find out if issue has a proposed resolution
   if (is_active(is.stat)) {
      auto k2 = tx.find("<resolution>", 0);
      if (k2 == std::string::npos) {
         is.has_resolution = false;
      }
      else {
         k2 += sizeof("<resolution>") - 1;
         auto l2 = tx.find("</resolution>", k2);
         is.has_resolution = l2 - k2 > 15;
      }
   }
   else {
      is.has_resolution = true;
   }

   return is;
}


auto read_issues(std::string const & issues_path) -> std::vector<issue> {
   std::cout << "Reading issues from: " << issues_path << std::endl;

   DIR* dir = opendir(issues_path.c_str());
   if (dir == 0) {
      throw std::runtime_error{"Unable to open issues dir"};
   }

   std::vector<issue> issues{};
   while ( dirent* entry = readdir(dir) ) {
      std::string const issue_file{ entry->d_name };
      if (0 != issue_file.find("issue") ) {
         continue;
      }

      issues.push_back(parse_issue_from_file(issues_path + issue_file));
   }
   closedir(dir);
   return issues;
}


void prepare_issues(std::vector<issue> & issues) {
   // Initially sort the issues by issue number, so each issue can be correctly 'format'ted
   sort(issues.begin(), issues.end(), sort_by_num{});

   // Then we format the issues, which should be the last time we need to touch the issues themselves
   // We may turn this into a two-stage process, analysing duplicates and then applying the links
   // This will allow us to better express constness when the issues are used purely for reference.
   // Currently, the 'format' function takes a reference-to-non-const-vector-of-issues purely to
   // mark up information related to duplicates, so processing duplicates in a separate pass may
   // clarify the code.
   for( auto & i : issues ) { format(issues, i); }

   // Issues will be routinely re-sorted in later code, but contents should be fixed after formatting.
   // This suggests we may want to be storing some kind of issue handle in the functions that keep
   // re-sorting issues, and so minimize the churn on the larger objects.
}


// ============================================================================================================


auto prepare_issues_for_diff_report(std::vector<issue> const & issues) -> std::vector<std::pair<int, std::string> > {
   std::vector<std::pair<int, std::string> > result;
   std::transform( issues.begin(), issues.end(), back_inserter(result),
                   [](issue const & iss) {
                      return std::make_pair(iss.num, iss.stat);
                   }
                 );
   return result;
}

auto read_issues_from_toc(std::string const & s) -> std::vector<std::pair<int, std::string> > {
   // parse all issues from the specified stream, 'is'.
   // Throws 'runtime_error' if *any* parse step fails
   //
   // We assume 'is' refers to a "toc" html document, for either the current or a previous issues list.
   // The TOC file consists of a sequence of HTML <tr> elements - each element is one issue/row in the table
   //    First we read the whole stream into a single 'string'
   //    Then we search the string for the first <tr> marker
   //       The first row is the title row and does not contain an issue.
   //       If cannt find the first row, we flag an error and exit
   //    Next we loop through the string, searching for <tr> markers to indicate the start of each issue
   //       We parse the issue number and status from each row, and append a record to the result vector
   //       If any parse fails, throw a runtime_error
   //    If debugging, display the results to 'cout'

   // Skip the title row
   auto i = s.find("<tr>");
   if (std::string::npos == i) {
      throw std::runtime_error{"Unable to find the first (title) row"};
   }

   // Read all issues in table
   std::vector<std::pair<int, std::string> > issues;
   for(;;) {
      i = s.find("<tr>", i+4);
      if (i == std::string::npos) {
         break;
      }
      i = s.find("</a>", i);
      auto j = s.rfind('>', i);
      if (j == std::string::npos) {
         throw std::runtime_error{"unable to parse issue number: can't find beginning bracket"};
      }
      std::istringstream instr{s.substr(j+1, i-j-1)};
      int num;
      instr >> num;
      if (instr.fail()) {
         throw std::runtime_error{"unable to parse issue number"};
      }
      i = s.find("</a>", i+4);
      if (i == std::string::npos) {
         throw std::runtime_error{"partial issue found"};
      }
      j = s.rfind('>', i);
      if (j == std::string::npos) {
         throw std::runtime_error{"unable to parse issue status: can't find beginning bracket"};
      }
      issues.push_back({num, s.substr(j+1, i-j-1)});
   }

   //display_issues(issues);
   return issues;
}


struct list_issues {
   std::vector<int> const & issues;
};


auto operator<<( std::ostream & out, list_issues const & x) -> std::ostream & {
   auto list_separator = "";
   for (auto number : x.issues ) {
      out << list_separator << "<iref ref=\"" << number << "\"/>";
      list_separator = ", ";
   }
   return out;
}


struct find_num {
   constexpr bool operator()(std::pair<int, std::string> const & x, int y) const noexcept {
      return x.first < y;
   }
};


struct discover_new_issues {
   std::vector<std::pair<int, std::string> > const & old_issues;
   std::vector<std::pair<int, std::string> > const & new_issues;
};


auto operator<<( std::ostream & out, discover_new_issues const & x) -> std::ostream & {
   std::vector<std::pair<int, std::string> > const & old_issues = x.old_issues;
   std::vector<std::pair<int, std::string> > const & new_issues = x.new_issues;

   std::map<std::string, std::vector<int> > added_issues;
   for( auto const & i : new_issues ) {
      auto j = std::lower_bound(old_issues.cbegin(), old_issues.cend(), i.first, find_num{});
      if(j == old_issues.end()) {
         added_issues[i.second].push_back(i.first);
      }
   }

   for( auto const & i : added_issues ) {
      auto const item_count = i.second.size();
      if(1 == item_count) {
         out << "<li>Added the following " << i.first << " issue: <iref ref=\"" << i.second.front() << "\"/>.</li>\n";
      }
      else {
         out << "<li>Added the following " << item_count << " " << i.first << " issues: " << list_issues{i.second} << ".</li>\n";
      }
   }

   return out;
}


struct reverse_pair {
   // member template means we cannot make thi a local class
   template <class T, class U>
   constexpr
   auto operator()(T const & x, U const & y) const noexcept -> bool {
      return static_cast<bool>(  x.second < y.second  or
                              (!(y.second < x.second)  and  x.first < y.first));
   }
};

struct discover_changed_issues {
   std::vector<std::pair<int, std::string> > const & old_issues;
   std::vector<std::pair<int, std::string> > const & new_issues;
};


auto operator<<( std::ostream & out, discover_changed_issues x) -> std::ostream & {
   std::vector<std::pair<int, std::string> > const & old_issues = x.old_issues;
   std::vector<std::pair<int, std::string> > const & new_issues = x.new_issues;

   std::map<std::pair<std::string, std::string>, std::vector<int>, reverse_pair> changed_issues;
   for (auto const & i : new_issues ) {
      auto j = std::lower_bound(old_issues.begin(), old_issues.end(), i.first, find_num{});
      if (j != old_issues.end()  and  i.first == j->first  and  j->second != i.second) {
         changed_issues[make_pair(j->second, i.second)].push_back(i.first);
      }
   }

   for (auto const & i : changed_issues ) {
      auto const item_count = i.second.size();
      if(1 == item_count) {
         out << "<li>Changed the following issue from " << i.first.first << " to " << i.first.second
             << ": <iref ref=\"" << i.second.front() << "\"/>.</li>\n";
      }
      else {
         out << "<li>Changed the following " << item_count << " issues from " << i.first.first << " to " << i.first.second
             << ": " << list_issues{i.second}
             << ".</li>\n";
      }
   }

   return out;
}


void count_issues(std::vector<std::pair<int, std::string> > const & issues, unsigned & n_open, unsigned & n_closed) {
   n_open = 0;
   n_closed = 0;

   for(auto const & elem : issues) {
      if (is_active(elem.second)) {
         ++n_open;
      }
      else {
         ++n_closed;
      }
   }
}


struct write_summary {
   std::vector<std::pair<int, std::string> > const & old_issues;
   std::vector<std::pair<int, std::string> > const & new_issues;
};


auto operator<<( std::ostream & out, write_summary const & x) -> std::ostream & {
   std::vector<std::pair<int, std::string> > const & old_issues = x.old_issues;
   std::vector<std::pair<int, std::string> > const & new_issues = x.new_issues;

   unsigned n_open_new = 0;
   unsigned n_open_old = 0;
   unsigned n_closed_new = 0;
   unsigned n_closed_old = 0;
   count_issues(old_issues, n_open_old, n_closed_old);
   count_issues(new_issues, n_open_new, n_closed_new);

   out << "<li>" << n_open_new << " open issues, ";
   if (n_open_new >= n_open_old) {
      out << "up by " << n_open_new - n_open_old << ".</li>\n";
   }
   else {
      out << "down by " << n_open_old - n_open_new << ".</li>\n";
   }

   out << "<li>" << n_closed_new << " closed issues, ";
   if (n_closed_new >= n_closed_old) {
      out << "up by " << n_closed_new - n_closed_old << ".</li>\n";
   }
   else {
      out << "down by " << n_closed_old - n_closed_new << ".</li>\n";
   }

   unsigned n_total_new = n_open_new + n_closed_new;
   unsigned n_total_old = n_open_old + n_closed_old;
   out << "<li>" << n_total_new << " issues total, ";
   if (n_total_new >= n_total_old) {
      out << "up by " << n_total_new - n_total_old << ".</li>\n";
   }
   else {
      out << "down by " << n_total_old - n_total_new << ".</li>\n";
   }

   return out;
}


void print_current_revisions( std::ostream & out
                            , std::vector<std::pair<int, std::string> > const & old_issues
                            , std::vector<std::pair<int, std::string> > const & new_issues
                            ) {
   out << "<ul>\n"
       << "<li><b>Summary:</b><ul>\n"
       << write_summary{old_issues, new_issues}
       << "</ul></li>\n"
       << "<li><b>Details:</b><ul>\n"
       << discover_new_issues{old_issues, new_issues}
       << discover_changed_issues{old_issues, new_issues}
       << "</ul></li>\n"
       << "</ul>\n";
}


// ============================================================================================================


int main(int argc, char* argv[]) {
   try {
      std::string path;
      std::cout << "Preparing new EWG issues lists..." << std::endl;
      if (argc == 2) {
         path = argv[1];
      }
      else {
         char cwd[1024];
         if (getcwd(cwd, sizeof(cwd)) == 0) {
            std::cout << "unable to getcwd\n";
            return 1;
         }
         path = cwd;
      }

      if (path.back() != '/') { path += '/'; }
      section_db  = read_section_db(path + "meta-data/");
      //    check_against_index(section_db);

      auto const old_issues = read_issues_from_toc(read_file_into_string(path + "meta-data/ewg-toc.old.html"));

      auto const issues_path = path + "xml/";
      EwgIssuesXml ewg_issues_xml(issues_path);

      auto issues = read_issues(issues_path);
      prepare_issues(issues);

      // issues must be sorted by number before making the mailing list documents
      //sort(issues.begin(), issues.end(), sort_by_num{});

      // Collect a report on all issues that have changed status
      // This will be added to the revision history of the 3 standard documents
      auto const new_issues = prepare_issues_for_diff_report(issues);

      std::ostringstream os_diff_report;
      print_current_revisions(os_diff_report, old_issues, new_issues );
      auto const diff_report = os_diff_report.str();

      std::vector<issue> unresolved_issues;
      std::vector<issue> votable_issues;

      std::copy_if(issues.begin(), issues.end(), std::back_inserter(unresolved_issues), [](issue const & iss){ return is_not_resolved(iss.stat); } );
      std::copy_if(issues.begin(), issues.end(), std::back_inserter(votable_issues),    [](issue const & iss){ return is_votable(iss.stat); } );

      // If votable list is empty, we are between meetings and should list Ready issues instead
      // Otherwise, issues moved to Ready during a meeting will remain 'unresolved' by that meeting
      auto ready_inserter = votable_issues.empty()
                          ? std::back_inserter(votable_issues)
                          : std::back_inserter(unresolved_issues);
      std::copy_if(issues.begin(), issues.end(), ready_inserter, [](issue const & iss){ return is_ready(iss.stat); } );

      // First generate the primary 3 standard issues lists
      make_active(issues, path, ewg_issues_xml, diff_report);
      make_defect(issues, path, ewg_issues_xml, diff_report);
      make_closed(issues, path, ewg_issues_xml, diff_report);

      // unofficial documents
      make_tentative(issues, path, ewg_issues_xml);
      make_unresolved(issues, path, ewg_issues_xml);
      make_immediate(issues, path, ewg_issues_xml);


      // Now we have a parsed and formatted set of issues, we can write the standard set of HTML documents
      // Note that each of these functions is going to re-sort the 'issues' vector for its own purposes
      make_sort_by_num            (issues, {path + "ewg-toc.html"},         ewg_issues_xml);
      make_sort_by_status         (issues, {path + "ewg-status.html"},      ewg_issues_xml);
      make_sort_by_status_mod_date(issues, {path + "ewg-status-date.html"}, ewg_issues_xml);
      make_sort_by_section        (issues, {path + "ewg-index.html"},       ewg_issues_xml);

      // Note that this additional document is very similar to unresolved-index.html below
      make_sort_by_section        (issues, {path + "ewg-index-open.html"},  ewg_issues_xml, true);

      // Make a similar set of index documents for the issues that are 'live' during a meeting
      // Note that these documents want to reference each other, rather than ewg- equivalents,
      // although it may not be worth attempting fix-ups as the per-issue level
      // During meetings, it would be good to list newly-Ready issues here
      make_sort_by_num            (unresolved_issues, {path + "unresolved-toc.html"},         ewg_issues_xml);
      make_sort_by_status         (unresolved_issues, {path + "unresolved-status.html"},      ewg_issues_xml);
      make_sort_by_status_mod_date(unresolved_issues, {path + "unresolved-status-date.html"}, ewg_issues_xml);
      make_sort_by_section        (unresolved_issues, {path + "unresolved-index.html"},       ewg_issues_xml);

      // Make another set of index documents for the issues that are up for a vote during a meeting
      // Note that these documents want to reference each other, rather than ewg- equivalents,
      // although it may not be worth attempting fix-ups as the per-issue level
      // Between meetings, it would be good to list Ready issues here
      make_sort_by_num            (votable_issues, {path + "votable-toc.html"},         ewg_issues_xml);
      make_sort_by_status         (votable_issues, {path + "votable-status.html"},      ewg_issues_xml);
      make_sort_by_status_mod_date(votable_issues, {path + "votable-status-date.html"}, ewg_issues_xml);
      make_sort_by_section        (votable_issues, {path + "votable-index.html"},       ewg_issues_xml);

      std::cout << "Made all documents\n";
   }
   catch(std::exception const & ex) {
      std::cout << ex.what() << std::endl;
      return 1;
   }
}
