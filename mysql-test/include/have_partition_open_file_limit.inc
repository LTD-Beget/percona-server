--disable_query_log
--exec ulimit -n > $MYSQL_TMP_DIR/osfileslimit.txt
CREATE TABLE t_o_f_l (c1 varchar(20));
eval LOAD DATA INFILE '$MYSQL_TMP_DIR/osfileslimit.txt' INTO TABLE t_o_f_l;
let $f_l=`SELECT c1 FROM t_o_f_l`;
--remove_file $MYSQL_TMP_DIR/osfileslimit.txt
DROP TABLE t_o_f_l;
if ($f_l < 8500)
{
  skip Need open_files_limit >= 8500 (see ulimit -n);
}
--enable_query_log