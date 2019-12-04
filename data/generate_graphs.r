#!/usr/bin/env Rscript
require(docopt)
library(ggplot2)
library(reshape2)
'Usage:
   generate_graphs.R [<filename>]

Options:
   --lxlim=<ns>      Left limit of x-axis on histogram [default: 0]
   --rxlim=<ns>      Right limit of x-axis on histogram [default: 10000]
   --lthres=<ns>     Data to cut off if less than [default: 0]
   --rthres=<ns>     Data to cut off if greater than [default: 100000]
   --diffthres=<ns>  Data to cut off if difference is greater than [default: 100000]

 ]' -> doc

opts <- docopt(doc)
opts$lxlim <- as.numeric(opts$lxlim)
opts$rxlim <- as.numeric(opts$rxlim)
opts$lthres <- as.numeric(opts$lthres)
opts$rthres <- as.numeric(opts$rthres)
opts$diffthres <- as.numeric(opts$diffthres)
# print(opts)

first.read.col <- 3
second.read.col <- 4

datatype <- "nanoseconds"

data <- read.csv(opts$filename)

df.orig <- data.frame(first_read=data[,first.read.col], second_read=data[,second.read.col])

df <- subset(df.orig, first_read > opts$lthres & second_read > opts$lthres)
df <- subset(df, first_read < opts$rthres & second_read < opts$rthres)
df <- subset(df, abs(first_read - second_read) < opts$diffthres)

print(paste("Filtered", nrow(df.orig) - nrow(df), "rows."), sep="")

histogram = ggplot(melt(df), aes(x = value, fill = variable)) +
    geom_density(alpha=0.5) +
    xlim(opts$lxlim, opts$rxlim) +
    labs(title=opts$filename, x=datatype, fill="read type")

ggsave(paste(strsplit(opts$filename, "\\.")[[1]][1], "-histogram.png", sep=""))

df$id <- seq_len(nrow(df))
df$diff <- df$first_read - df$second_read

# print(head(df))

barchart = ggplot(data=df, aes(x = id, y = diff)) +
    geom_bar(stat="identity", aes(fill=diff)) +
#    scale_fill_gradient2(low="red", high="green", mid="black") +
    labs(title=opts$filename, x="read-write-read iteration", y=paste(datatype, " diff", sep=""))

ggsave(paste(strsplit(opts$filename, "\\.")[[1]][1], "-barchart.png", sep=""))